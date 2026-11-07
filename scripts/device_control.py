#!/usr/bin/env python3
"""Drive a CrossPoint reader over USB serial, for tests and measurements.

Talks to the firmware's serial console (src/SerialConsole.cpp): each command is
sent as "CMD:<NAME> ..." and answered with "CMD_OK <NAME> ..." or
"CMD_ERR <NAME> <reason>". Every line the reader prints can be kept in a log
file, across restarts (the USB port disappears and comes back on each one).

Examples:
    device_control.py state
    device_control.py click confirm
    device_control.py open --cold "/Books/Some Book.epub"
    device_control.py restart --wait
    device_control.py screenshot screen.png
    device_control.py --log build.log wait "Page 50 processed" --timeout 120
    device_control.py --log experiment.log run experiment.txt

A run script holds one command per line, in the same form as on the command
line ("#" starts a comment). The commands share one connection and one log,
so a script can restart the reader, open a book cold and wait for a log line:

    restart --wait
    open --cold "/Books/Some Book.epub"
    wait "Page 100 processed" --timeout 180
    state

Requires pyserial (scripts/requirements.txt).
"""

from __future__ import annotations

import argparse
import glob
import re
import shlex
import struct
import sys
import time
import zlib
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - reported at run time
    serial = None

ESPRESSIF_USB_VID = 0x303A


class DeviceError(Exception):
    pass


def find_port() -> str | None:
    if serial is not None:
        ports = sorted(list_ports.comports(), key=lambda p: p.device)
        for port in ports:
            if port.vid == ESPRESSIF_USB_VID:
                return port.device
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyACM*"):
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    return None


class Device:
    """A serial link that reconnects when the reader restarts, and logs every line."""

    def __init__(self, port: str | None, log_path: str | None, echo: bool):
        self.requested_port = port
        self.log = open(log_path, "ab") if log_path else None
        self.echo = echo
        self.link = None
        self.pending = b""

    def close(self) -> None:
        if self.link is not None:
            self.link.close()
            self.link = None
        if self.log is not None:
            self.log.close()

    def _connect(self, deadline: float) -> bool:
        while time.time() < deadline:
            port = self.requested_port or find_port()
            if port:
                try:
                    self.link = serial.Serial(port, 115200, timeout=0.1)
                    return True
                except (serial.SerialException, OSError):
                    pass
            time.sleep(0.25)
        return False

    def _record(self, data: bytes) -> None:
        if self.log is not None:
            self.log.write(data)
            self.log.flush()

    def read_bytes(self, deadline: float) -> bytes:
        """Whatever arrives before the deadline, reconnecting if the port drops."""
        if self.link is None and not self._connect(deadline):
            return b""
        try:
            data = self.link.read(4096)
        except (serial.SerialException, OSError):
            self.link.close()
            self.link = None
            self.pending = b""  # never join half a line from the old boot to the new one
            self._record(b"\n[device_control: port dropped, reconnecting]\n")
            return b""
        self._record(data)
        if self.echo and data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
        return data

    def read_line(self, deadline: float) -> str | None:
        while b"\n" not in self.pending:
            if time.time() >= deadline:
                return None
            self.pending += self.read_bytes(deadline)
        line, self.pending = self.pending.split(b"\n", 1)
        return line.decode("utf-8", "replace").rstrip("\r")

    def read_exact(self, count: int, deadline: float) -> bytes:
        while len(self.pending) < count:
            if time.time() >= deadline:
                raise DeviceError(f"timed out after {len(self.pending)} of {count} bytes")
            self.pending += self.read_bytes(deadline)
        data, self.pending = self.pending[:count], self.pending[count:]
        return data

    def send(self, line: str, deadline: float) -> None:
        while time.time() < deadline:
            if self.link is None and not self._connect(deadline):
                break
            try:
                self.link.write(f"CMD:{line}\n".encode())
                self.link.flush()
                return
            except (serial.SerialException, OSError):
                self.link.close()
                self.link = None
        raise DeviceError("no reader on USB serial")

    def command(self, line: str, timeout: float = 5.0, busy_retry: float = 5.0) -> str:
        """Send one command and return its CMD_OK detail; retry while the reader is busy."""
        name = line.split(" ", 1)[0]
        reply = re.compile(rf"CMD_(OK|ERR) {re.escape(name)}(?: (.*))?$")
        retry_until = time.time() + busy_retry
        while True:
            deadline = time.time() + timeout
            self.send(line, deadline)
            while True:
                text = self.read_line(deadline)
                if text is None:
                    raise DeviceError(f"no reply to {name} within {timeout:g} s")
                match = reply.search(text)
                if match:
                    break
            status, detail = match.group(1), match.group(2) or ""
            if status == "OK":
                return detail
            if detail.startswith("busy") and time.time() < retry_until:
                time.sleep(0.2)
                continue
            raise DeviceError(f"{name}: {detail}")

    def wait_for(self, pattern: str, timeout: float) -> str:
        expr = re.compile(pattern)
        deadline = time.time() + timeout
        while True:
            text = self.read_line(deadline)
            if text is None:
                raise DeviceError(f"{pattern!r} did not appear within {timeout:g} s")
            if expr.search(text):
                return text

    def capture(self, seconds: float) -> None:
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.read_bytes(deadline)

    def wait_until_up(self, timeout: float) -> str:
        """After a restart: PING until the new boot answers."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                return self.command("PING", timeout=1.0, busy_retry=0)
            except DeviceError:
                continue
        raise DeviceError(f"the reader did not answer within {timeout:g} s of the restart")

    def screenshot(self, timeout: float = 15.0) -> bytes:
        deadline = time.time() + timeout
        self.send("SCREENSHOT", deadline)
        while True:
            text = self.read_line(deadline)
            if text is None:
                raise DeviceError("no screenshot")
            if "SCREENSHOT_START:" in text:
                size = int(text.rsplit(":", 1)[1])
                break
        data = self.read_exact(size, deadline)
        self.wait_for("SCREENSHOT_END", max(1.0, deadline - time.time()))
        return data


def framebuffer_to_png(data: bytes, width: int, height: int, rotate: int) -> bytes:
    """1-bit framebuffer (set bit = white, rows of `width` pixels) to a grayscale PNG."""
    row_bytes = width // 8
    if len(data) != row_bytes * height:
        raise DeviceError(f"framebuffer is {len(data)} bytes, not {width}x{height} at 1 bit per pixel")
    pixels = [[(data[y * row_bytes + x // 8] >> (7 - x % 8)) & 1 for x in range(width)] for y in range(height)]
    for _ in range((rotate // 90) % 4):  # clockwise quarter turns
        pixels = [list(row) for row in zip(*pixels[::-1])]
    out_h, out_w = len(pixels), len(pixels[0])
    raw = bytearray()
    for row in pixels:
        raw.append(0)  # filter: none
        packed = bytearray((out_w + 7) // 8)
        for x, bit in enumerate(row):
            if bit:
                packed[x // 8] |= 0x80 >> (x % 8)
        raw += packed

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", out_w, out_h, 1, 0, 0, 0, 0)  # 1-bit grayscale
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(
        b"IEND", b""
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="serial port (default: the first Espressif USB device)")
    parser.add_argument("--log", help="append every line the reader prints to this file")
    parser.add_argument("--echo", action="store_true", help="also print the reader's output")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("ping", help="check the link")
    sub.add_parser("state", help="screen in front, open book and heap")
    p = sub.add_parser("click", help="one click on any screen")
    p.add_argument("button", choices=["confirm", "back", "next", "prev"])
    p = sub.add_parser("turn", help="a page or chapter turn in the reader")
    p.add_argument("direction", choices=["next", "prev"])
    p.add_argument("--chapter", action="store_true")
    sub.add_parser("menu", help="open the reader menu")
    sub.add_parser("home", help="go back to Home")
    p = sub.add_parser("open", help="open a book by its path on the SD card")
    p.add_argument("path")
    p.add_argument("--cold", action="store_true", help="drop its laid-out chapters first, so they are rebuilt")
    p = sub.add_parser("restart", help="reboot the reader")
    p.add_argument("target", nargs="?", choices=["home", "reader"], help="silently into Home or the open book")
    p.add_argument("--wait", action="store_true", help="return once the new boot answers")
    p.add_argument("--timeout", type=float, default=30.0)
    p = sub.add_parser("screenshot", help="save the screen as a PNG")
    p.add_argument("output")
    p.add_argument("--size", default="800x480", help="framebuffer WIDTHxHEIGHT (default: 800x480, the X4)")
    p.add_argument("--rotate", type=int, default=90, help="clockwise degrees (default: 90, portrait)")
    p = sub.add_parser("wait", help="wait for a log line matching a regular expression")
    p.add_argument("pattern")
    p.add_argument("--timeout", type=float, default=60.0)
    p = sub.add_parser("log", help="record the reader's output for a while")
    p.add_argument("seconds", type=float)
    p = sub.add_parser("sleep", help="pause a run script, still recording")
    p.add_argument("seconds", type=float)
    p = sub.add_parser("run", help="run commands from a file, one per line")
    p.add_argument("script")
    return parser


def execute(device: Device, args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    cmd = args.command
    if cmd == "ping":
        print(device.command("PING"))
    elif cmd == "state":
        print(device.command("STATE"))
    elif cmd == "click":
        device.command(f"CLICK {args.button.upper()}")
    elif cmd == "turn":
        device.command(f"TURN {args.direction.upper()}" + (" CHAPTER" if args.chapter else ""))
    elif cmd == "menu":
        device.command("MENU")
    elif cmd == "home":
        device.command("HOME", timeout=15.0)  # answered once Home has painted
    elif cmd == "open":
        device.command("OPEN " + ("COLD " if args.cold else "") + args.path)
    elif cmd == "restart":
        device.command("RESTART" + (f" {args.target.upper()}" if args.target else ""))
        if args.wait:
            time.sleep(1.0)  # let the old boot go before asking the new one
            print(device.wait_until_up(args.timeout))
    elif cmd == "screenshot":
        width, height = (int(v) for v in args.size.lower().split("x"))
        Path(args.output).write_bytes(framebuffer_to_png(device.screenshot(), width, height, args.rotate))
        print(args.output)
    elif cmd == "wait":
        print(device.wait_for(args.pattern, args.timeout))
    elif cmd in ("log", "sleep"):
        device.capture(args.seconds)
    elif cmd == "run":
        # Check every line before the first step runs, so a typo on line 8 does
        # not surface after a restart and a long wait on lines 1-7.
        steps = []
        for number, raw in enumerate(Path(args.script).read_text().splitlines(), 1):
            try:
                words = shlex.split(raw, comments=True)
            except ValueError as error:
                raise DeviceError(f"{args.script}:{number}: {error}") from None
            if not words:
                continue
            try:
                step = parser.parse_args(words)
            except SystemExit:  # argparse has already printed why
                raise DeviceError(f"{args.script}:{number}: invalid step") from None
            if step.command == "run":
                raise DeviceError(f"{args.script}:{number}: run scripts do not nest")
            steps.append((number, raw, step))
        for number, raw, step in steps:
            print(f"> {raw.strip()}", flush=True)
            try:
                execute(device, step, parser)
            except DeviceError as error:
                raise DeviceError(f"{args.script}:{number}: {error}") from None


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    if serial is None:
        print("device_control.py needs pyserial: pip install -r scripts/requirements.txt", file=sys.stderr)
        return 2
    device = Device(args.port, args.log, args.echo)
    try:
        execute(device, args, parser)
    except DeviceError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    finally:
        device.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
