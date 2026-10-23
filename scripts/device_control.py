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
    device_control.py wait "Page 50 processed" --timeout 120 --log build.log
    device_control.py run experiment.txt --log experiment.log
    device_control.py push book.epub /Books
    device_control.py install-font fonts/NotoSerif
    device_control.py ls /Books
    device_control.py pull "/Books/Some Book.epub" copy.epub

A run script holds one command per line, in the same form as on the command
line ("#" starts a comment). The commands share one connection and one log,
so a script can restart the reader, open a book cold and wait for a log line:

    restart --wait
    open --cold "/Books/Some Book.epub"
    wait "Page 100 processed" --timeout 180
    state

File commands (push, install-font, rm, mkdir) need the reader on Home; push
and install-font take --home to go there first. Files move in CRC-checked
frames (src/util/SerialTransfer.h) and land under their name only once
complete. Requires pyserial (scripts/requirements.txt).
"""

from __future__ import annotations

import argparse
import glob
import re
import shlex
import struct
import sys
import time
import unicodedata
import zlib
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - reported at run time
    serial = None

ESPRESSIF_USB_VID = 0x303A

# Frame header of the file commands (src/util/SerialTransfer.h): magic, offset, length, CRC.
FRAME = struct.Struct("<2sIHI")
FRAME_MAGIC = b"\xfe\x5a"
FRAME_MAX_PAYLOAD = 4096
CANCEL_OFFSET = 0xFFFFFFFF  # a frame at this offset, with no payload, ends a PUT
FRAME_GAP = 0.1  # the reader's FRAME_GAP_MS: a pause this long inside a frame drops it
# Matches the reader's HOST_TIMEOUT_MS: no answer for this long means it has gone.
ACK_TIMEOUT = 5.0
MAX_NAKS_IN_A_ROW = 10
# A removal or rename on the reader waits for any cover thumbnail Home is building.
CHANGE_TIMEOUT = 60.0


class DeviceError(Exception):
    pass


class DeviceRefused(DeviceError):
    """The reader answered CMD_ERR: the command is over on its side."""


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
        self.resent = 0  # frames sent or fetched again, for the transfer summary
        self.window = 2  # PUT frames in flight

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
            # Whatever is waiting, else block for the first byte: a short line is not held for the timeout.
            data = self.link.read(max(1, self.link.in_waiting))
        except (serial.SerialException, OSError):
            self.link.close()
            self.link = None
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

    def command(self, line: str, timeout: float = 5.0, busy_retry: float = 5.0, lines: list | None = None) -> str:
        """Send one command and return its CMD_OK detail; retry while the reader is busy.

        If `lines` is a list, the command's own output lines ("<NAME>_...") are added to it.
        """
        name = line.split(" ", 1)[0]
        reply = re.compile(rf"CMD_(OK|ERR) {re.escape(name)}(?: (.*))?$")
        retry_until = time.time() + busy_retry
        while True:
            deadline = time.time() + timeout
            self.send(line, deadline)
            if lines is not None:
                lines.clear()
            while True:
                text = self.read_line(deadline)
                if text is None:
                    raise DeviceError(f"no reply to {name} within {timeout:g} s")
                match = reply.search(text)
                if match:
                    break
                if lines is not None and text.startswith(f"{name}_"):
                    lines.append(text)
            status, detail = match.group(1), match.group(2) or ""
            if status == "OK":
                return detail
            if detail.startswith("busy") and time.time() < retry_until:
                time.sleep(0.2)
                continue
            raise DeviceRefused(f"{name}: {detail}")

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


    def go_home(self, timeout: float = 15.0) -> None:
        """Back to Home, and wait until it is in front."""
        self.command("HOME")
        deadline = time.time() + timeout
        while "activity=Home " not in self.command("STATE"):
            if time.time() >= deadline:
                raise DeviceError("Home did not come up")
            time.sleep(0.2)

    def list_dir(self, remote: str) -> list[tuple[str, int, str]]:
        """(kind, size, name) of each entry; kind is "D" for a folder, "F" for a file."""
        lines: list[str] = []
        self.command(f"LS {remote}", timeout=30.0, lines=lines)
        entries = []
        for text in lines:
            kind, size, name = text.split(" ", 3)[1:]
            entries.append((kind, int(size), name))
        return entries

    def _reply(self, name: str, deadline: float, prefixes: tuple[str, ...]) -> tuple[str, str]:
        """The next "<prefix> ..." line or CMD_OK/CMD_ERR reply of command `name`: (prefix or status, rest)."""
        reply = re.compile(rf"CMD_(OK|ERR) {re.escape(name)}(?: (.*))?$")
        while True:
            text = self.read_line(deadline)
            if text is None:
                raise DeviceError(f"{name}: the reader stopped answering")
            match = reply.search(text)
            if match:
                if match.group(1) == "ERR":
                    raise DeviceRefused(f"{name}: {match.group(2) or ''}")
                return "OK", match.group(2) or ""
            for prefix in prefixes:
                if text.startswith(prefix + " "):
                    return prefix, text[len(prefix) + 1 :]

    def put(self, data: bytes, remote: str, replace: bool, progress=None) -> str:
        """Send a file in frames; returns where the reader stored it."""
        crc = zlib.crc32(data) & 0xFFFFFFFF
        line = f"PUT {'REPLACE ' if replace else ''}{len(data)} {crc:08x} {remote}"
        retry_until = time.time() + 5.0
        while True:
            self.send(line, time.time() + ACK_TIMEOUT)
            try:
                kind, rest = self._reply("PUT", time.time() + ACK_TIMEOUT, ("PUT_READY",))
                break
            except DeviceError as error:
                if str(error) != "PUT: busy" or time.time() >= retry_until:
                    raise
                time.sleep(0.2)
        if kind == "OK":
            raise DeviceError("PUT: the reader did not wait for the data")
        chunk = min(FRAME_MAX_PAYLOAD, int(rest))
        # The reader's receive path is lossless (HalSerialBulkRead), so a second frame can wait
        # in the USB pipe while it writes the first; every frame gets exactly one reply.
        try:
            self._send_frames(data, chunk, progress)
        except DeviceRefused:
            raise
        except (DeviceError, KeyboardInterrupt):
            # End the transfer now rather than leave the reader waiting for its timeout. Wait out
            # its in-frame gap first, so a frame cut short by Ctrl-C is dropped and cannot swallow this.
            if self.link is not None:
                time.sleep(FRAME_GAP + 0.1)
                self.link.write(FRAME.pack(FRAME_MAGIC, CANCEL_OFFSET, 0, 0))
                self.link.flush()
            raise
        # The rename waits for any cover the reader is building on Home.
        return self._reply("PUT", time.time() + CHANGE_TIMEOUT, ())[1]

    def _send_frames(self, data: bytes, chunk: int, progress) -> None:
        def send_frame(offset: int) -> int:
            payload = data[offset : offset + chunk]
            crc = zlib.crc32(payload, zlib.crc32(struct.pack("<IH", offset, len(payload)))) & 0xFFFFFFFF
            self.link.write(FRAME.pack(FRAME_MAGIC, offset, len(payload), crc) + payload)
            return offset + len(payload)

        acked = sent = 0
        in_flight = 0
        naks = 0
        while acked < len(data):
            while in_flight < self.window and sent < len(data):
                sent = send_frame(sent)
                in_flight += 1
            kind, rest = self._reply("PUT", time.time() + ACK_TIMEOUT, ("PUT_ACK", "PUT_NAK"))
            if kind == "OK":
                raise DeviceError("PUT: the reader finished early")
            in_flight -= 1
            words = rest.split()
            offset = int(words[0])
            if not 0 <= offset <= len(data):
                raise DeviceError(f"PUT: the reader asked for offset {offset}")
            if kind == "PUT_ACK":
                naks = 0
                acked = offset
            else:
                naks += 1
                self.resent += 1
                if naks > MAX_NAKS_IN_A_ROW:
                    raise DeviceError(f"PUT: frames keep failing ({' '.join(words[1:])})")
                # Frames sent after the refused one are refused too; collect those replies, then resume.
                while in_flight > 0:
                    self._reply("PUT", time.time() + ACK_TIMEOUT, ("PUT_ACK", "PUT_NAK"))
                    in_flight -= 1
                acked = sent = offset
            if progress:
                progress(acked)

    def get(self, remote: str, progress=None) -> bytes:
        """Fetch a file; a damaged or stalled stream is resumed from the last good frame."""
        data = bytearray()
        size = None
        for _attempt in range(4):
            try:
                # Any input, this GET included, stops a stream the reader is still sending.
                size = int(self.command(f"GET {len(data)} {remote}", timeout=10.0))
                while len(data) < size:
                    offset, payload = self._read_frame(time.time() + 2.0)
                    if offset != len(data):
                        raise DeviceError(f"frame at {offset}, expected {len(data)}")
                    data += payload
                    if progress:
                        progress(len(data), size)
                return bytes(data)
            except DeviceRefused:
                raise
            except DeviceError as error:
                self.resent += 1
                self._record(f"\n[device_control: GET resumes at {len(data)}: {error}]\n".encode())
                self.capture(0.5)  # let the rest of the broken stream drain
                self.pending = b""
        raise DeviceError(f"GET {remote}: failed at {len(data)} of {size} bytes")

    def _read_frame(self, deadline: float) -> tuple[int, bytes]:
        """The next frame in the stream; log text before it is skipped."""
        while True:
            start = self.pending.find(FRAME_MAGIC)
            if start < 0:
                if b"GET_ABORT" in self.pending:
                    raise DeviceError("the reader stopped sending")
                self.pending = self.pending[-1:]
            elif len(self.pending) >= start + FRAME.size:
                _magic, offset, length, crc = FRAME.unpack_from(self.pending, start)
                end = start + FRAME.size + length
                if length > FRAME_MAX_PAYLOAD:
                    raise DeviceError("damaged frame header")
                if len(self.pending) >= end:
                    payload = self.pending[start + FRAME.size : end]
                    self.pending = self.pending[end:]
                    if zlib.crc32(payload, zlib.crc32(struct.pack("<IH", offset, length))) & 0xFFFFFFFF != crc:
                        raise DeviceError("frame CRC mismatch")
                    return offset, payload
            if time.time() >= deadline:
                raise DeviceError("the stream stalled")
            self.pending += self.read_bytes(deadline)


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


class Progress:
    """One status line per file, and the throughput once it is done."""

    def __init__(self, label: str, total: int):
        self.label, self.total = label, total
        self.start = time.time()
        self.live = sys.stderr.isatty()

    def __call__(self, done: int, total: int | None = None) -> None:
        if total is not None:
            self.total = total
        if self.live:
            elapsed = max(time.time() - self.start, 1e-6)
            percent = 100 * done // self.total if self.total else 100
            sys.stderr.write(f"\r{self.label}  {percent:3d}%  {done / 1024:,.0f} KB  {done / 1024 / elapsed:,.0f} KB/s ")
            sys.stderr.flush()

    def finish(self, note: str = "") -> float:
        elapsed = max(time.time() - self.start, 1e-6)
        if self.live:
            sys.stderr.write("\r\033[K")
        rate = self.total / 1024 / elapsed
        print(f"{self.label}: {self.total:,} bytes in {elapsed:.2f} s ({rate:,.1f} KB/s){note}")
        return elapsed


def remote_join(folder: str, name: str) -> str:
    # NFC, as the web upload does: a Mac hands out decomposed names that FAT tools then miss.
    name = unicodedata.normalize("NFC", name)
    return folder.rstrip("/") + "/" + name


def remote_path(path: str) -> str:
    path = unicodedata.normalize("NFC", path)
    return path.rstrip("/") or "/"


def push_file(device: Device, local: Path, remote: str, replace: bool) -> int:
    data = local.read_bytes()
    progress = Progress(remote, len(data))
    resent = device.resent
    stored = device.put(data, remote, replace, progress)
    retries = device.resent - resent
    progress.finish((f" -> {stored}" if stored != remote else "") + (f", {retries} frames resent" if retries else ""))
    return len(data)


def push(device: Device, local: Path, remote_dir: str, replace: bool) -> None:
    if remote_dir != "/":
        device.command(f"MKDIR {remote_dir}")
    if local.is_file():
        push_file(device, local, remote_join(remote_dir, local.name), replace)
        return
    if not local.is_dir():
        raise DeviceError(f"{local}: no such file or folder")
    total, start = 0, time.time()
    base = remote_join(remote_dir, local.name)
    device.command(f"MKDIR {base}")
    for path in sorted(local.rglob("*")):
        parts = path.relative_to(local).parts
        if any(part.startswith(".") for part in parts):  # .DS_Store, ._ resource forks
            continue
        remote = base
        for part in parts:
            remote = remote_join(remote, part)
        if path.is_dir():
            device.command(f"MKDIR {remote}")
        else:
            total += push_file(device, path, remote, replace)
    elapsed = max(time.time() - start, 1e-6)
    print(f"{local.name}: {total:,} bytes in {elapsed:.1f} s ({total / 1024 / elapsed:,.1f} KB/s)")


FONT_SUFFIXES = (".cpfont", ".ttf", ".otf")


def font_family(file_name: str) -> str:
    """The family the web Fonts page would give this file: the name before its last - or _."""
    stem = re.sub(r"\.(cpfont|ttf|otf)$", "", file_name, flags=re.IGNORECASE)
    cut = max(stem.rfind("-"), stem.rfind("_"))
    family = stem[:cut] if cut > 0 else stem
    return re.sub(r"[^A-Za-z0-9_-]", "_", family)


def install_font(device: Device, local: Path, family: str | None, replace: bool) -> None:
    files = [local] if local.is_file() else sorted(p for p in local.iterdir() if p.is_file())
    files = [p for p in files if p.suffix.lower() in FONT_SUFFIXES and not p.name.startswith(".")]
    if not files:
        raise DeviceError(f"{local}: no {', '.join(FONT_SUFFIXES)} files")
    if family is None:
        families = {font_family(p.name) for p in files}
        if len(families) != 1:
            raise DeviceError(f"files of several families ({', '.join(sorted(families))}); pass --family")
        family = families.pop()
    for path in files:
        # The reader keeps font names to A-Z, a-z, 0-9, - and _; the web upload turns spaces into _ too.
        push_file(device, path, f"/.fonts/{family}/{path.name.replace(' ', '_')}", replace)
    print(f"font family {family}: {len(files)} file{'s' if len(files) != 1 else ''}")


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
    p = sub.add_parser("ls", help="list a folder on the SD card")
    p.add_argument("path", nargs="?", default="/")
    p = sub.add_parser("push", help="copy a file or folder to the SD card (reader on Home)")
    p.add_argument("local")
    p.add_argument("remote_dir", nargs="?", default="/", help="folder on the card (default: /), made if missing")
    p.add_argument("--replace", action="store_true", help="overwrite files that already exist")
    p.add_argument("--home", action="store_true", help="go to Home first")
    p = sub.add_parser("install-font", help="copy one font family's files into the fonts folder (reader on Home)")
    p.add_argument("local", help="a font file, or a folder of one family's files")
    p.add_argument("--family", help="family name (default: from the file names, as the web Fonts page does)")
    p.add_argument("--replace", action="store_true", help="overwrite files that already exist")
    p.add_argument("--home", action="store_true", help="go to Home first")
    p = sub.add_parser("pull", help="copy a file from the SD card")
    p.add_argument("path")
    p.add_argument("local", nargs="?", help="where to save it (default: its name, here)")
    p.add_argument("--force", action="store_true", help="overwrite a local file")
    p = sub.add_parser("mkdir", help="make a folder on the SD card (reader on Home)")
    p.add_argument("path")
    p = sub.add_parser("rm", help="remove a file, an empty folder or /.fonts/<family> (reader on Home)")
    p.add_argument("path")
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
        device.command("HOME")
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
    elif cmd == "ls":
        for kind, size, name in sorted(device.list_dir(remote_path(args.path)), key=lambda e: (e[0] != "D", e[2])):
            print(f"{'-' if kind == 'D' else f'{size:,}':>14}  {name}{'/' if kind == 'D' else ''}")
    elif cmd == "push":
        if args.home:
            device.go_home()
        push(device, Path(args.local), remote_path(args.remote_dir), args.replace)
    elif cmd == "install-font":
        if args.home:
            device.go_home()
        install_font(device, Path(args.local), args.family, args.replace)
    elif cmd == "pull":
        remote = remote_path(args.path)
        local = Path(args.local or remote.rsplit("/", 1)[1])
        if local.exists() and not args.force:
            raise DeviceError(f"{local} exists (--force overwrites)")
        progress = Progress(remote, 0)
        data = device.get(remote, progress)
        local.write_bytes(data)
        progress.finish(f" -> {local}")
    elif cmd == "mkdir":
        device.command(f"MKDIR {remote_path(args.path)}")
    elif cmd == "rm":
        device.command(f"RM {remote_path(args.path)}", timeout=CHANGE_TIMEOUT)
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
