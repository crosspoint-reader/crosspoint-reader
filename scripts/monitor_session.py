"""Single-owner serial transport shared by the monitor's UI and HTTP clients."""

from __future__ import annotations

import json
import queue
import re
import secrets
import threading
import time
from collections import deque
from concurrent.futures import Future, TimeoutError
from pathlib import Path

from monitor_screenshot import framebuffer_to_pbm


class SessionError(Exception):
    def __init__(self, message, status=409):
        super().__init__(message)
        self.status = status


def open_serial(port, baud):
    import serial

    # Set requested levels before opening; USB drivers may still reset the device.
    connection = serial.Serial(port=None, baudrate=baud, timeout=0.05, write_timeout=1)
    connection.dtr = False
    connection.rts = False
    if hasattr(connection, "exclusive"):
        connection.exclusive = True
    connection.port = port
    try:
        connection.open()
    except Exception:
        connection.close()
        raise
    return connection


class DeviceSession:
    MAX_SCREENSHOT = 2 * 1024 * 1024
    SCREENSHOT_TIMEOUT = 10

    def __init__(
        self,
        port,
        baud,
        output_dir,
        serial_factory=open_serial,
        event_capacity=4096,
    ):
        self.port = port
        self.baud = baud
        self.output_dir = Path(output_dir).resolve()
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.serial_factory = serial_factory
        self.condition = threading.Condition(threading.RLock())
        self.events = deque(maxlen=event_capacity)
        self.sequence = 0
        self.state = "disconnected"
        self.last_error = None
        self.lease_token = None
        self.lease_until = 0
        self.jobs = queue.Queue(maxsize=64)
        self.stopped = threading.Event()
        self.thread = threading.Thread(
            target=self._run, name="device-serial", daemon=True
        )
        self.connection = None
        self.paused = False
        self.pending = bytearray()
        self.capture = None
        self.capture_metadata = None
        self.capture_size = 0
        self.capture_deadline = 0
        self.capture_id = None
        self.waiting_screenshot = False
        self.command_id = 0
        self.raw_log = None
        self.event_log = None

    def start(self):
        self.raw_log = (self.output_dir / "serial.bin").open("ab", buffering=0)
        try:
            self.event_log = (self.output_dir / "events.jsonl").open(
                "a", encoding="utf-8", buffering=1
            )
            self.thread.start()
        except Exception:
            self.raw_log.close()
            if self.event_log:
                self.event_log.close()
            raise

    def close(self):
        self.stopped.set()
        with self.condition:
            self.condition.notify_all()
        if self.thread.ident is not None:
            self.thread.join()
        for stream in (self.raw_log, self.event_log):
            if stream:
                stream.close()

    def _publish(self, kind, **fields):
        with self.condition:
            self.sequence += 1
            event = dict(seq=self.sequence, time=time.time(), type=kind, **fields)
            if self.event_log:
                self.event_log.write(json.dumps(event, ensure_ascii=True) + "\n")
            self.events.append(event)
            self.condition.notify_all()
            return event

    def status(self):
        with self.condition:
            return {
                "state": self.state,
                "port": self.port,
                "baud": self.baud,
                "seq": self.sequence,
                "error": self.last_error,
                "output_dir": str(self.output_dir),
                "control_claimed": self.lease_until > time.monotonic(),
            }

    def read_events(self, after=0, wait=0):
        deadline = time.monotonic() + wait
        with self.condition:
            while self.sequence <= after and not self.stopped.is_set():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                self.condition.wait(remaining)
            oldest = self.events[0]["seq"] if self.events else self.sequence + 1
            return {
                "events": [e for e in self.events if e["seq"] > after],
                "cursor": self.sequence,
                "dropped": after < oldest - 1,
            }

    def _check_control(self, token):
        if self.lease_until > time.monotonic() and token != self.lease_token:
            raise SessionError("Device control is claimed by another client")
        if token and (
            token != self.lease_token or self.lease_until <= time.monotonic()
        ):
            raise SessionError("Control token is invalid or expired")

    def claim(self, token=None, ttl=60):
        if (
            isinstance(ttl, bool)
            or not isinstance(ttl, (int, float))
            or not 1 <= ttl <= 300
        ):
            raise SessionError("ttl must be between 1 and 300 seconds", 400)
        with self.condition:
            self._check_control(token)
            self.lease_token = token or secrets.token_urlsafe(24)
            self.lease_until = time.monotonic() + ttl
            return {"token": self.lease_token, "ttl": ttl}

    def unclaim(self, token):
        with self.condition:
            if not token:
                raise SessionError("A control token is required", 400)
            self._check_control(token)
            self.lease_token = None
            self.lease_until = 0
            return {"released": True}

    def request(self, action, command=None, token=None):
        if action not in ("command", "release", "reconnect"):
            raise SessionError("Unknown action", 400)
        if action == "command":
            if (
                not isinstance(command, str)
                or not command.strip()
                or len(command.encode("utf-8")) > 256
                or any(ord(c) < 32 for c in command)
            ):
                raise SessionError(
                    "command must be a single line of 1–256 UTF-8 bytes", 400
                )
            command = command.strip()
            command = command.removeprefix("CMD:")
            if not command:
                raise SessionError("Empty command", 400)
        future = Future()
        with self.condition:
            self._check_control(token)
            if self.stopped.is_set():
                raise SessionError("Session is stopped", 503)
            try:
                self.jobs.put_nowait((future, action, command, token))
            except queue.Full:
                raise SessionError("Command queue is full", 503) from None
        try:
            return future.result(timeout=3)
        except TimeoutError:
            cancelled = future.cancel()
            raise SessionError(
                "Request timed out; "
                + (
                    "cancelled before execution"
                    if cancelled
                    else "delivery is uncertain; do not retry blindly"
                ),
                504,
            ) from None

    def _set_state(self, state, error=None):
        with self.condition:
            changed = (state, error) != (self.state, self.last_error)
            self.state, self.last_error = state, error
            if changed:
                self._publish("connection", state=state, error=error)

    def _disconnect(self):
        if self.connection:
            try:
                self.connection.close()
            finally:
                self.connection = None
        if self.waiting_screenshot or self.capture is not None:
            self._publish(
                "screenshot_error",
                command_id=self.capture_id,
                error="Transfer interrupted",
            )
        self.pending.clear()
        self.capture = None
        self.capture_metadata = None
        self.capture_id = None
        self.waiting_screenshot = False

    def _execute(self, action, command):
        if action == "release":
            self.paused = True
            self._disconnect()
            self._set_state("released")
            return self.status()
        if action == "reconnect":
            self._disconnect()
            self.paused = False
            self._set_state("disconnected")
            return self.status()
        if self.connection is None:
            raise SessionError("Device is not connected", 503)
        if self.waiting_screenshot or self.capture is not None:
            raise SessionError("Screenshot transfer is in progress")
        self.command_id += 1
        command_id = self.command_id
        after = self.sequence
        payload = ("CMD:" + command + "\n").encode("utf-8")
        try:
            if self.connection.write(payload) != len(payload):
                raise OSError("Incomplete serial write")
        except OSError as exc:
            self._disconnect()
            self._set_state("disconnected", str(exc))
            raise SessionError(
                "Serial write failed; delivery is uncertain", 503
            ) from exc
        if command == "SCREENSHOT":
            self.capture_metadata = None
            self.waiting_screenshot = True
            self.capture_id = command_id
            self.capture_deadline = time.monotonic() + self.SCREENSHOT_TIMEOUT
        self._publish("command_sent", command_id=command_id, command=command)
        return {"command_id": command_id, "after": after, "sent": True}

    def _process_job(self):
        try:
            future, action, command, token = self.jobs.get_nowait()
        except queue.Empty:
            return
        if not future.set_running_or_notify_cancel():
            return
        try:
            # A lease may have changed while this request was queued.
            with self.condition:
                self._check_control(token)
                result = self._execute(action, command)
            future.set_result(result)
        except Exception as exc:  # noqa: BLE001 - return worker failures to the requesting client.
            future.set_exception(exc)

    def _save_capture(self):
        name = f"screenshot-{self.sequence + 1}"
        raw_path = self.output_dir / (name + ".raw")
        raw_path.write_bytes(self.capture)
        fields = {
            "command_id": self.capture_id,
            "raw_path": str(raw_path),
            "size": len(self.capture),
        }
        metadata = self.capture_metadata
        path = self.output_dir / (name + ".pbm")
        pbm, width, height = framebuffer_to_pbm(self.capture, **metadata)
        path.write_bytes(pbm)
        fields.update(
            image_path=str(path),
            width=width,
            height=height,
            physical_width=metadata["width"],
            physical_height=metadata["height"],
            stride=metadata["stride"],
            rotation=metadata["rotation"],
            inverted=bool(metadata["inverted"]),
        )
        self._publish("screenshot", **fields)
        self.capture = None
        self.capture_metadata = None
        self.capture_id = None
        self.waiting_screenshot = False

    def _feed(self, data):
        self.pending.extend(data)
        while self.pending:
            if self.capture is not None and len(self.capture) < self.capture_size:
                count = min(self.capture_size - len(self.capture), len(self.pending))
                self.capture.extend(self.pending[:count])
                del self.pending[:count]
                continue
            newline = self.pending.find(b"\n")
            if newline < 0:
                if len(self.pending) > 16384:
                    raise OSError("Serial line exceeds 16 KiB; resynchronizing")
                return
            line = bytes(self.pending[: newline + 1])
            del self.pending[: newline + 1]
            text = line.decode("utf-8", errors="replace").rstrip("\r\n")
            if self.capture is not None:
                if text != "SCREENSHOT_END":
                    raise OSError(
                        "Invalid screenshot trailer; possible interleaved logs"
                    )
                self._save_capture()
                continue
            if text.startswith("SCREENSHOT_META:"):
                match = re.fullmatch(
                    r"SCREENSHOT_META:1:(\d+):(\d+):(\d+):(0|90|180|270):(0|1)", text
                )
                if not match:
                    raise OSError("Invalid screenshot metadata")
                width, height, stride, rotation, inverted = map(int, match.groups())
                if not (
                    width > 0
                    and height > 0
                    and stride >= (width + 7) // 8
                    and stride * height <= self.MAX_SCREENSHOT
                ):
                    raise OSError("Invalid screenshot geometry")
                self.capture_metadata = {
                    "width": width,
                    "height": height,
                    "stride": stride,
                    "rotation": rotation,
                    "inverted": inverted,
                }
                self.waiting_screenshot = True
                self.capture_deadline = time.monotonic() + self.SCREENSHOT_TIMEOUT
                continue
            if text.startswith("SCREENSHOT_START:"):
                match = re.fullmatch(r"SCREENSHOT_START:(\d+)", text)
                if not match or not 0 < int(match[1]) <= self.MAX_SCREENSHOT:
                    raise OSError("Invalid screenshot size")
                self.capture_size = int(match[1])
                if self.capture_metadata is None:
                    raise OSError("Screenshot requires SCREENSHOT_META version 1")
                if self.capture_size != (
                    self.capture_metadata["stride"] * self.capture_metadata["height"]
                ):
                    raise OSError("Screenshot size does not match metadata")
                self.capture = bytearray()
                self.capture_deadline = time.monotonic() + self.SCREENSHOT_TIMEOUT
                continue
            self._publish("log", line=text)
            match = re.search(r"\[CTL\] (id=\d+ boot=\d+ event=[A-Z_]+(?: .*)?)$", text)
            if match:
                fields = {}
                for item in match[1].split():
                    if "=" in item:
                        key, value = item.split("=", 1)
                        fields[key] = (
                            int(value) if re.fullmatch(r"-?\d+", value) else value
                        )
                self._publish("control", **fields)
                if (
                    fields.get("event") == "ERROR"
                    and fields.get("id") == 0
                    and self.waiting_screenshot
                ):
                    self._publish(
                        "screenshot_error",
                        command_id=self.capture_id,
                        error=fields.get("reason", "Firmware rejected screenshot"),
                    )
                    self.waiting_screenshot = False
                    self.capture_id = None
                    self.capture_metadata = None

    def _run(self):
        retry_at = 0
        try:
            while not self.stopped.is_set():
                self._process_job()
                if self.paused:
                    self.stopped.wait(0.05)
                    continue
                try:
                    if self.connection is None:
                        if time.monotonic() < retry_at:
                            self.stopped.wait(0.05)
                            continue
                        self.connection = self.serial_factory(self.port, self.baud)
                        self._set_state("connected")
                    data = self.connection.read(
                        min(max(self.connection.in_waiting, 1), 4096)
                    )
                    if data:
                        self.raw_log.write(data)
                        self._feed(data)
                    if (
                        self.waiting_screenshot or self.capture is not None
                    ) and time.monotonic() > self.capture_deadline:
                        raise OSError("Screenshot timed out")
                except OSError as exc:
                    self._disconnect()
                    self._set_state("disconnected", str(exc))
                    retry_at = time.monotonic() + 1
        finally:
            self.stopped.set()
            self._disconnect()
            self._set_state("stopped")
            while not self.jobs.empty():
                future, *_ = self.jobs.get_nowait()
                if future.set_running_or_notify_cancel():
                    future.set_exception(SessionError("Session stopped", 503))
