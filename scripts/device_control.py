"""Serial UI control through the debugging monitor's localhost HTTP API."""

import argparse
import io
import json
import math
import os
import shlex
import sys
import tempfile
import time
from contextlib import contextmanager
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen


class ControlError(RuntimeError):
    def __init__(self, message, code=4):
        super().__init__(message)
        self.code = code


class ControlTimeout(ControlError, TimeoutError):
    def __init__(self, message):
        super().__init__(message, 5)


def serial_command(command):
    command = command.removeprefix("CMD:")
    if (
        not command.strip()
        or len(command) + 4 > 127
        or any(ord(c) < 32 or ord(c) > 126 for c in command)
    ):
        raise ControlError(
            "Command must be one ASCII line of at most 127 bytes including CMD:", 2
        )
    return command


def screenshot_path(path):
    path = Path(path)
    if path.suffix.lower() not in (".pbm", ".png", ".raw"):
        raise ControlError("Screenshot path must end in .pbm, .png, or .raw", 2)
    return path


class DeviceControl:
    """Hold a monitor lease; fail commands rather than replaying uncertain input."""

    def __init__(self, base="http://127.0.0.1:8765"):
        self.base = base.rstrip("/")
        self.token = None
        self.boot = None
        self.next_id = 1
        self.last_state = None
        self.last_command = None
        self.output_dir = None
        self.cursor = None
        self._deadline = None
        self._renew_at = 0
        self._synced = False
        self._press_pending = False

    @contextmanager
    def operation(self, timeout):
        previous = self._deadline
        deadline = time.monotonic() + timeout
        self._deadline = min(previous, deadline) if previous is not None else deadline
        try:
            yield
        finally:
            self._deadline = previous

    def remaining(self):
        if self._deadline is None:
            return 8.0
        remaining = self._deadline - time.monotonic()
        if remaining <= 0:
            raise ControlTimeout(
                "Operation timed out; input already sent may have executed"
            )
        return remaining

    def api(self, path, body=None, binary=False):
        headers = {"Content-Type": "application/json"}
        if self.token:
            headers["X-Control-Token"] = self.token
        request = Request(
            self.base + path,
            None if body is None else json.dumps(body).encode(),
            headers,
        )
        try:
            with urlopen(request, timeout=min(8, self.remaining())) as response:
                payload = response.read()
                return payload if binary else json.loads(payload)
        except HTTPError as exc:
            try:
                message = json.loads(exc.read()).get("error", str(exc))
            except (ValueError, OSError):
                message = str(exc)
            if "uncertain" in message or exc.code == 504:
                raise ControlTimeout(message) from exc
            raise ControlError(message, 3 if exc.code == 503 else 4) from exc
        except (URLError, OSError) as exc:
            if isinstance(exc, TimeoutError) or isinstance(
                getattr(exc, "reason", None), TimeoutError
            ):
                raise ControlTimeout(
                    "HTTP request timed out; delivery may be uncertain"
                ) from exc
            raise ControlError(f"Monitor connection failed: {exc}", 3) from exc
        except ValueError as exc:
            raise ControlError("Monitor returned invalid JSON", 3) from exc

    def status(self):
        result = self.api("/status")
        self.output_dir = result["output_dir"]
        return result

    def claim(self):
        if self.token is None or time.monotonic() >= self._renew_at:
            self.token = self.api("/control/claim", {"ttl": 300})["token"]
            self._renew_at = time.monotonic() + 30

    def close(self):
        try:
            if self.token:
                # Cleanup has its own short deadline, even after an operation timeout.
                previous = self._deadline
                self._deadline = time.monotonic() + 2
                try:
                    self.api("/control/release", {})
                finally:
                    self._deadline = previous
        finally:
            self.token = None
            self._synced = False

    def __enter__(self):
        try:
            self.claim()
            self.info()
            return self
        except BaseException:
            self.__exit__(*sys.exc_info())
            raise

    def __exit__(self, exc_type, _exc, _traceback):
        try:
            self.close()
        except ControlError:
            if exc_type is None:
                raise

    def _check_event(self, event):
        if event["type"] == "connection" and event["state"] != "connected":
            raise ControlError(
                "Serial disconnected; input already sent may have executed", 3
            )
        if self.boot is not None:
            if event["type"] == "control" and event["boot"] != self.boot:
                raise ControlError(
                    "Device rebooted; establish a new control session", 6
                )
            if event["type"] == "log" and (
                event["line"].startswith("ESP-ROM:")
                or "abort() was called" in event["line"]
                or event["line"].strip() == "Rebooting..."
            ):
                raise ControlError(
                    "Device crashed or rebooted; stopping the control session", 6
                )

    def events(self, cursor, wait=1):
        wait = max(0, min(wait, self.remaining() - 0.1))
        batch = self.api(f"/events?after={cursor}&wait={wait:.3f}")
        self.cursor = batch["cursor"]
        if batch["dropped"]:
            raise ControlError(
                "Control event history gap; delivery may be uncertain", 6
            )
        # Check the entire batch before returning any successful reply from it.
        for event in batch["events"]:
            self._check_event(event)
        return batch

    def _exchange(self, command, request_id, expected, timeout):
        with self.operation(timeout):
            self.claim()
            self.last_command = {
                "command": command,
                "request_id": request_id,
                "stage": "sending",
            }
            sent = self.api("/command", {"command": command})
            self.last_command.update(command_id=sent["command_id"], stage="written")
            cursor = sent["after"]
            while True:
                batch = self.events(cursor)
                cursor = batch["cursor"]
                for event in batch["events"]:
                    if event["type"] != "control" or event["id"] != request_id:
                        continue
                    self.last_command["stage"] = event["event"].lower()
                    if event["event"] == "ERROR":
                        raise ControlError(
                            "Firmware rejected command: " + str(event.get("reason"))
                        )
                    if event["event"] == "CANCELLED" and expected != "CANCELLED":
                        raise ControlError("Firmware cancelled the input command")
                    if event["event"] == expected:
                        return event

    def info(self):
        result = self._exchange("INFO 0", 0, "INFO", 10)
        if result.get("protocol") != 1:
            raise ControlError("Unsupported firmware control protocol")
        self.boot = result["boot"]
        self.next_id = result["last_id"] + 1
        self._synced = True
        return result

    def _command(self, verb, args="", expected="STATE", timeout=10):
        with self.operation(timeout):
            if not self._synced:
                self.info()
            request_id = self.next_id
            self.next_id += 1
            if request_id > 0xFFFFFFFF:
                raise ControlError("Firmware request IDs exhausted; reboot the device")
            return self._exchange(
                f"{verb} {request_id} {args}".strip(), request_id, expected, timeout
            )

    def state(self):
        self.last_state = self._command("STATE")
        return self.last_state

    def press(self, button, hold_ms=80, timeout=15):
        self._press_pending = True
        result = self._command("PRESS", f"{button} {hold_ms}", "INPUT_DONE", timeout)
        self._press_pending = False
        return result

    def cancel(self):
        result = self._command("CANCEL", expected="CANCELLED")
        self._press_pending = False
        return result

    def wait_ready(self, predicate=None, timeout=60):
        """Wait without requesting a redraw; use a predicate to check the result."""
        with self.operation(timeout):
            ready = False
            while True:
                if self._deadline <= time.monotonic():
                    if ready and predicate is not None:
                        raise ControlError(
                            "Ready UI did not match the expected state", 7
                        )
                    raise ControlTimeout("UI did not become ready")
                state = self.state()
                ready = state.get("available") and not state.get("busy")
                if ready and (predicate is None or predicate(state)):
                    return state
                # Classify expiry between polls using the confirmed state.
                # Timeouts raised inside state() still mean an unanswered query.
                time.sleep(min(0.1, max(0, self._deadline - time.monotonic())))

    def send(self, command):
        command = serial_command(command)
        self.claim()
        self.last_command = {"command": command, "stage": "sending"}
        result = self.api("/command", {"command": command})
        self.last_command.update(command_id=result["command_id"], stage="written")
        # A raw command may consume firmware IDs; rediscover before typed commands.
        self._synced = False
        return dict(result, stage="written", execution_confirmed=False)

    def serial(self, action):
        self.claim()
        result = self.api("/serial/" + action, {})
        self.boot = None
        self.last_state = None
        self._synced = False
        if action == "reconnect":
            while result["state"] != "connected":
                time.sleep(min(0.1, self.remaining()))
                result = self.status()
        return result

    def screenshot(self, path):
        path = screenshot_path(path)
        image_module = None
        if path.suffix.lower() == ".png":
            try:
                from PIL import Image

                image_module = Image
            except ImportError as exc:
                raise ControlError(
                    "PNG output requires Pillow; use .pbm without extra dependencies", 2
                ) from exc
        # Validate destination before sending commands; publish only complete files.
        with tempfile.NamedTemporaryFile(
            dir=path.parent, prefix=".device-", delete=False
        ) as stream:
            temporary = Path(stream.name)
        try:
            self.wait_ready()
            self.claim()
            sent = self.api("/screenshot", {})
            self.last_command = {
                "command": "SCREENSHOT",
                "command_id": sent["command_id"],
                "stage": "written",
            }
            cursor = sent["after"]
            while True:
                batch = self.events(cursor)
                cursor = batch["cursor"]
                capture = None
                for event in batch["events"]:
                    if event.get("command_id") != sent["command_id"]:
                        continue
                    if event["type"] == "screenshot_error":
                        raise ControlError(
                            "Screenshot failed: " + str(event.get("error"))
                        )
                    if event["type"] == "screenshot":
                        capture = event
                if capture is not None:
                    break
            key = "raw_path" if path.suffix.lower() == ".raw" else "image_path"
            payload = self.api(
                "/screenshots/" + quote(Path(capture[key]).name), binary=True
            )
            if image_module:
                with image_module.open(io.BytesIO(payload)) as image:
                    image.save(temporary, format="PNG")
            else:
                temporary.write_bytes(payload)
            os.replace(temporary, path)
            self.last_command["stage"] = "captured"
            return dict(capture, path=str(path.resolve()))
        finally:
            temporary.unlink(missing_ok=True)


def positive(value):
    value = float(value)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("must be finite and greater than zero")
    return value


def integer(minimum, maximum):
    def parse(value):
        number = int(value)
        if not minimum <= number <= maximum:
            raise argparse.ArgumentTypeError(f"must be between {minimum} and {maximum}")
        return number

    return parse


class Parser(argparse.ArgumentParser):
    def error(self, message):
        raise ControlError(f"{self.prog}: {message}", 2)


def parser():
    result = Parser(description=__doc__)

    def globals_for(target, suppress=False):
        def default(value):
            return argparse.SUPPRESS if suppress else value

        target.add_argument(
            "--url", default=default("http://127.0.0.1:8765"), help="monitor API URL"
        )
        target.add_argument(
            "--json",
            action="store_true",
            default=default(False),
            help="emit JSON; streams use JSON Lines",
        )
        target.add_argument(
            "--timeout",
            type=positive,
            default=default(60),
            help="operation deadline in seconds (default: 60)",
        )

    globals_for(result)
    sub = result.add_subparsers(dest="command", required=True)

    def command(name, help_text):
        child = sub.add_parser(name, help=help_text)
        globals_for(child, True)
        return child

    command("status", "show monitor connection and log location")
    command("info", "query firmware protocol and boot identity")
    command("state", "query activity, reader position, and readiness")
    press = command("press", "press a logical button; wait for input and readiness")
    press.add_argument(
        "button",
        type=lambda s: s.upper().replace("-", "_"),
        choices=(
            "BACK",
            "CONFIRM",
            "LEFT",
            "RIGHT",
            "UP",
            "DOWN",
            "NAV_NEXT",
            "NAV_PREVIOUS",
            "PAGE_FORWARD",
            "PAGE_BACK",
        ),
    )
    press.add_argument("--hold-ms", type=integer(20, 2000), default=80)
    press.add_argument("--repeat", type=integer(1, 10000), default=1)
    press.add_argument("--wait", choices=("ready", "input"), default="ready")
    press.add_argument("--expect-page-change", action="store_true")
    press.add_argument("--expect-activity")
    wait = command("wait", "wait for ready UI and optional state conditions")
    wait.add_argument("--activity")
    wait.add_argument("--spine", type=integer(0, 65535))
    wait.add_argument("--page", type=integer(1, 65535))
    command("cancel", "cancel pending or held synthetic input")
    screenshot = command(
        "screenshot", "capture a host-rotated PBM/PNG or physical raw bytes"
    )
    screenshot.add_argument("path")
    send = command("send", "write a raw command; does not confirm execution")
    send.add_argument("text", help="quoted single command, with optional CMD: prefix")
    logs = command("logs", "read retained serial logs without claiming control")
    logs.add_argument("--follow", action="store_true")
    logs.add_argument("--after", type=integer(0, 2**63 - 1))
    logs.add_argument("--filter", default="", help="case-insensitive substring")
    serial = command("serial", "release the port or reconnect (may reset the device)")
    serial.add_argument("action", choices=("release", "reconnect"))
    run = command("run", "execute a command file under one lease; - reads stdin")
    run.add_argument("file")
    return result


def validate(args):
    if args.command == "send":
        serial_command(args.text)
    elif args.command == "screenshot":
        screenshot_path(args.path)
    if (
        args.command == "press"
        and args.wait == "input"
        and (args.expect_page_change or args.expect_activity)
    ):
        raise ControlError("Expectations require --wait ready", 2)


def plan(args, command_parser):
    validate(args)
    if args.command != "run":
        return [(None, args)]
    if args.file == "-":
        lines = sys.stdin.read().splitlines()
    else:
        lines = Path(args.file).read_text().splitlines()
    steps = []
    for number, line in enumerate(lines, 1):
        try:
            tokens = shlex.split(line, comments=True)
            if not tokens:
                continue
            if any(
                t in ("--json", "--url", "--help", "-h") or t.startswith("--url=")
                for t in tokens
            ):
                raise ControlError(
                    "Set output format and URL on the run command; help is not a step",
                    2,
                )
            step = command_parser.parse_args(["--timeout", str(args.timeout), *tokens])
            validate(step)
            if step.command == "run" or (step.command == "logs" and step.follow):
                raise ControlError(
                    "Nested run and logs --follow are not supported in command files", 2
                )
            steps.append((number, step))
        except (ValueError, ControlError) as exc:
            raise ControlError(f"Line {number}: {exc}", 2) from exc
    if not steps:
        raise ControlError("Command file contains no commands", 2)
    return steps


def execute(device, args):
    command = args.command
    if command == "status":
        yield device.status()
    elif command == "info":
        yield device.info()
    elif command == "state":
        yield device.state()
    elif command == "wait":
        conditions = {
            key: value
            for key, value in (
                ("activity", args.activity),
                ("spine", args.spine),
                ("page", args.page),
            )
            if value is not None
        }
        yield device.wait_ready(
            (lambda s: all(s.get(k) == v for k, v in conditions.items()))
            if conditions
            else None,
            args.timeout,
        )
    elif command == "press":
        for repeat in range(1, args.repeat + 1):
            before = device.wait_ready(timeout=args.timeout)
            if args.expect_page_change and not before.get("reader"):
                raise ControlError(
                    "Page-change expectation requires an active reader", 7
                )
            delivered = device.press(args.button, args.hold_ms, args.timeout)
            result = {"input": delivered, "repeat": repeat}
            if args.wait == "ready":

                def expected(state, before=before):
                    if (
                        args.expect_activity
                        and state.get("activity") != args.expect_activity
                    ):
                        return False
                    if args.expect_page_change:
                        return (
                            state.get("reader") == before["reader"]
                            and state["activity"] == before["activity"]
                            and (state["spine"], state["page"])
                            != (before["spine"], before["page"])
                        )
                    return True

                result["state"] = device.wait_ready(
                    expected
                    if args.expect_activity or args.expect_page_change
                    else None,
                    args.timeout,
                )
            yield result
    elif command == "cancel":
        yield device.cancel()
    elif command == "send":
        yield device.send(args.text)
    elif command == "serial":
        yield device.serial(args.action)
    elif command == "screenshot":
        yield device.screenshot(args.path)
    elif command == "logs":
        cursor = args.after if args.after is not None else 0
        first = True
        while True:
            # Follow streams run until Ctrl-C; timeout bounds each HTTP poll.
            with device.operation(args.timeout):
                batch = device.api(
                    f"/events?after={cursor}&wait={1 if args.follow else 0}"
                )
            if batch["dropped"]:
                if not first or args.after is not None:
                    raise ControlError(
                        "Log event history gap; consult the saved monitor log", 6
                    )
                print(
                    "Showing retained logs; older events remain in the monitor output directory.",
                    file=sys.stderr,
                )
            first = False
            cursor = batch["cursor"]
            for event in batch["events"]:
                if (
                    event["type"] == "log"
                    and args.filter.lower() in event["line"].lower()
                ):
                    yield event
            if not args.follow:
                break


def emit(value, as_json):
    if as_json:
        print(json.dumps(value, ensure_ascii=False), flush=True)
    elif value.get("type") == "log":
        print(value["line"], flush=True)
    elif value.get("ok") is False:
        print(f"Error: {value['error']}", file=sys.stderr)
        if value.get("output_dir"):
            print(f"Logs: {value['output_dir']}", file=sys.stderr)
    else:
        print(json.dumps(value, ensure_ascii=False, indent=2), flush=True)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    as_json = "--json" in argv
    device = None
    current = None
    line = None
    code = 0
    try:
        command_parser = parser()
        args = command_parser.parse_args(argv)
        as_json = args.json
        steps = plan(args, command_parser)
        device = DeviceControl(args.url)
        with device.operation(args.timeout):
            device.status()
        for line, current in steps:
            if current.command == "logs" and current.follow:
                for value in execute(device, current):
                    emit(value, as_json)
                continue
            with device.operation(current.timeout):
                # A batch keeps ownership even between read-only steps.
                if args.command == "run":
                    device.claim()
                for value in execute(device, current):
                    result = dict(value)
                    if args.command == "run":
                        result.update(step=line, command=current.command)
                    emit(result, as_json)
    except KeyboardInterrupt:
        code = 130
        if device and device.token and device.boot and device._press_pending:
            try:
                with device.operation(1):
                    device.cancel()
            except ControlError:
                pass
        emit({"ok": False, "error": "Interrupted", "exit_code": code}, as_json)
    except (ControlError, OSError, ValueError) as exc:
        code = exc.code if isinstance(exc, ControlError) else 1
        error = {"ok": False, "error": str(exc), "exit_code": code}
        if current:
            error["command"] = current.command
        if line is not None:
            error["step"] = line
        if device:
            error.update(
                boot=device.boot,
                last_command=device.last_command,
                last_state=device.last_state,
                cursor=device.cursor,
                output_dir=device.output_dir,
            )
        emit(error, as_json)
    finally:
        if device:
            try:
                device.close()
            except ControlError as exc:
                if not code:
                    code = exc.code
                    emit(
                        {
                            "ok": False,
                            "error": "Control lease cleanup failed: " + str(exc),
                            "exit_code": code,
                        },
                        as_json,
                    )
    return code


if __name__ == "__main__":
    sys.exit(main())
