#!/usr/bin/env python3
"""Drive the reader over USB serial with the serial test API (docs/contributing/serial-test-api.md).

    scripts/devctl.py [--port P] [--log FILE] [--rotate DEG|auto] STEP...
    scripts/devctl.py [--port P] --script steps.txt

Steps run in order; the run stops at the first failed step and exits non-zero (--keep-going runs
them all and exits non-zero if any failed).

  cmd:<TEXT>            send "CMD:<TEXT>", wait for its reply line, print it
                        (fails on a *_ERROR / *_TIMEOUT reply or none at all)
  btn:<NAME>[:<how>]    CMD:BTN:<NAME>[:<how>], then wait until the device is idle
  state                 CMD:STATE, print the reply and remember its fields for `check:`
  check:<key>=<regex>   the last STATE's <key> must match <regex> (re.fullmatch)
  idle[:<ms>]           CMD:IDLE[:<ms>], wait for its answer (IDLE_TIMEOUT or IDLE_ERROR fails it)
  shot:<file.png>       CMD:SCREENSHOT, decode the 1-bit frame, rotate it, save it (needs Pillow)
  wait:<seconds>        read (and log) the device's output for a while
  expect:<regex>:<sec>  read until an output line matches (re.search), fail otherwise

A --script file holds one step per line; blank lines and lines starting with # are skipped.
Every line the device prints goes to --log with a timestamp.

The port is opened with the default DTR/RTS lines, which does not reset the reader, and closed with
RTS dropped before DTR: RTS high with DTR low is esptool's reset request, which a plain close on
Windows can send. The device re-enumerates its USB port across a reboot; the tool reopens it and
carries on. A command whose write failed that way fails its step as DELIVERY UNCERTAIN and is not
sent again: the device may have run it.
"""
import argparse
import re
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")

# The device takes lines of up to 255 bytes with their line end; this tool sends "\r\n" on opening the
# port, so a first command must leave room for those two bytes as well.
MAX_LINE = 253
FAILED = re.compile(r"_ERROR|_TIMEOUT|^CMD_ERROR")
IDLE_DEFAULT_MS = 10000
# After a reply timed out, the next command waits until no line has come for RESYNC_QUIET_S (at most
# RESYNC_MAX_S): a late reply to the old command must not pass for a reply to the new one.
RESYNC_QUIET_S = 1.0
RESYNC_MAX_S = 10.0

# GfxRenderer draws LandscapeCounterClockwise in the panel's own frame; the others are turned.
# Degrees for PIL's rotate() (counter-clockwise) that turn the frame upright.
ROTATION = {"landscape_ccw": 0, "portrait": -90, "landscape_cw": 180, "portrait_inverted": 90}
# Panel sizes by frame size, for a release build's SCREENSHOT_START, which carries the size only.
PANELS = {48000: (800, 480), 52272: (792, 528), 52992: (768, 552)}


def split_fields(text):
    """key=value pairs of a STATE line; values in double quotes may hold spaces (\\ escapes)."""
    fields = {}
    i, n = 0, len(text)
    while i < n:
        while i < n and text[i] == " ":
            i += 1
        if i >= n:
            break
        eq = text.find("=", i)
        if eq < 0:
            break
        key = text[i:eq]
        i = eq + 1
        value = []
        if i < n and text[i] == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\" and i + 1 < n:
                    i += 1
                value.append(text[i])
                i += 1
            i += 1
        else:
            while i < n and text[i] != " ":
                value.append(text[i])
                i += 1
        fields[key] = "".join(value)
    return fields


def reply_family(text):
    """Replies to command `text` start with its name and '_' or ':' (IDLE:, IDLE_TIMEOUT:, ...).
    The name is taken as the device takes it, after spaces and tabs around the command."""
    return re.compile(re.escape(text.strip(" \t").split(":", 1)[0]) + "[_:]")


def is_reply_to(line, family):
    """Whether `line` answers a command of `family`. CMD_ERROR answers a line the device could not
    take as any command."""
    return family.match(line) is not None or line.startswith("CMD_ERROR")


class DeliveryUncertain(Exception):
    """Writing a command failed with the port: the device may or may not have received it."""


class Device:
    def __init__(self, port, log_path=None):
        self.port = port
        self.log = open(log_path, "a", encoding="utf-8", errors="replace") if log_path else None
        self.t0 = time.time()
        self.buf = b""
        self.unsynced = False  # True from a send until its reply is read: a late reply may still come
        self.ser = self._open()

    def note(self, line):
        if self.log:
            self.log.write(f"[{time.time() - self.t0:8.2f}] {line}\n")
            self.log.flush()

    def _open(self):
        for _ in range(60):
            try:
                s = serial.Serial(self.port, 115200, timeout=0.2)
                s.write(b"\r\n")  # some hosts get no output before they have written something
                return s
            except (serial.SerialException, OSError):
                time.sleep(0.5)
        sys.exit(f"cannot open {self.port}")

    def close(self):
        # RTS goes low before DTR: RTS high with DTR low is esptool's reset request, and a plain
        # close on Windows can drop DTR first.
        if self.ser.is_open:
            try:
                self.ser.rts = False
                self.ser.dtr = False
            except (serial.SerialException, OSError):
                pass  # a port without modem lines (a pty)
        self.ser.close()

    def _reopen(self):
        """The port went away (reboot, sleep, re-enumeration): reopen it."""
        try:
            self.close()
        except (serial.SerialException, OSError):
            pass
        time.sleep(1)
        self.ser = self._open()

    def _read(self, size):
        try:
            return self.ser.read(size)
        except (serial.SerialException, OSError):
            self._reopen()
            return b""

    def _read_some(self):
        try:
            waiting = self.ser.in_waiting
        except (serial.SerialException, OSError):
            waiting = 0
        return self._read(min(max(waiting, 1), 65536))

    def read_line(self, deadline):
        while time.time() < deadline:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line, self.buf = self.buf[:nl], self.buf[nl + 1:]
                text = line.decode("utf-8", errors="replace").rstrip("\r")
                self.note(text)
                return text
            self.buf += self._read_some()
        return None

    def read_exact(self, size, deadline):
        while len(self.buf) < size and time.time() < deadline:
            self.buf += self._read(min(65536, size - len(self.buf)))
        data, self.buf = self.buf[:size], self.buf[size:]
        return data

    def drain(self):
        """Log and drop the lines already received, so a late reply to an earlier command (one that
        timed out here) cannot pass for the reply to the next. A partial line is kept."""
        try:
            waiting = self.ser.in_waiting
        except (serial.SerialException, OSError):
            waiting = 0
        if waiting:
            self.buf += self._read(waiting)
        while (nl := self.buf.find(b"\n")) >= 0:
            line, self.buf = self.buf[:nl], self.buf[nl + 1:]
            self.note(line.decode("utf-8", errors="replace").rstrip("\r"))

    def resync(self):
        """Read (and log) until no line has come for RESYNC_QUIET_S, at most RESYNC_MAX_S."""
        end = time.time() + RESYNC_MAX_S
        quiet_until = time.time() + RESYNC_QUIET_S
        while time.time() < min(end, quiet_until):
            if self.read_line(min(end, quiet_until)) is not None:
                quiet_until = time.time() + RESYNC_QUIET_S
        self.unsynced = False

    def send(self, text):
        line = ("CMD:" + text).encode()
        if len(line) > MAX_LINE:
            raise ValueError(f"command longer than {MAX_LINE} bytes: {text[:40]}...")
        if self.unsynced:
            self.resync()
        self.drain()
        self.note(f">>> CMD:{text}")
        self.unsynced = True
        try:
            self.ser.write(line + b"\n")
        except (serial.SerialException, OSError) as error:
            self.note(f"write failed: {error}")
            self._reopen()
            raise DeliveryUncertain(f"CMD:{text} (port reopened, not resent)") from error

    def command(self, text, timeout=20):
        """Send a command and return its reply line, or None. Other lines are skipped: log lines and
        replies to other commands."""
        family = reply_family(text)
        self.send(text)
        deadline = time.time() + timeout
        while (line := self.read_line(deadline)) is not None:
            if is_reply_to(line, family):
                # A wait already running on the device still sends its own answer.
                self.unsynced = line == "IDLE_ERROR:busy"
                return line
        return None

    def state(self):
        reply = self.command("STATE")
        fields = split_fields(reply[len("STATE:"):]) if reply and reply.startswith("STATE:") else None
        return reply, fields

    def screenshot(self, path, rotate):
        if rotate == "auto":
            _, fields = self.state()
            rotate = ROTATION.get((fields or {}).get("orientation", ""), -90)
        self.send("SCREENSHOT")
        deadline = time.time() + 30
        while (line := self.read_line(deadline)) is not None:
            if line.startswith("SCREENSHOT_START:"):
                parts = line.split(":")
                size = int(parts[1])
                if len(parts) >= 4:
                    width, height = int(parts[2]), int(parts[3])
                elif size in PANELS:
                    width, height = PANELS[size]
                else:
                    return f"unknown panel for a {size}-byte frame"
                data = self.read_exact(size, time.time() + 30)
                if len(data) != size:
                    return f"short frame: {len(data)} of {size} bytes"
                from PIL import Image  # only shot: needs Pillow

                image = Image.frombytes("1", (width, height), data)
                if rotate:
                    image = image.rotate(rotate, expand=True)
                image.save(path)
                end = self.read_line(time.time() + 5)
                if end != "SCREENSHOT_END":
                    return f"no SCREENSHOT_END (got {end!r})"
                self.unsynced = False
                return None
            if line.startswith("SCREENSHOT_ERROR") or line.startswith("CMD_ERROR"):
                self.unsynced = False
                return line
        return "no reply"


def idle_timeout(arg):
    """Seconds to wait for the reply to CMD:IDLE[:<arg>]: the device's own timeout and some slack."""
    return (int(arg) if arg.isdigit() else IDLE_DEFAULT_MS) / 1000 + 5


def wait_idle(device, arg=""):
    reply = device.command("IDLE" + (":" + arg if arg else ""), timeout=idle_timeout(arg))
    print(reply if reply else "NO REPLY to IDLE")
    return bool(reply) and reply.startswith("IDLE:")


def run_step(device, step, rotate, fields):
    """Runs one step and returns whether it passed. `fields` keeps the last STATE's fields."""
    kind, _, rest = step.partition(":")
    if kind == "cmd":
        timeout = idle_timeout(rest[5:]) if rest.startswith("IDLE") else 20
        reply = device.command(rest, timeout=timeout)
        print(reply if reply else f"NO REPLY to {rest}")
        ok = bool(reply) and not FAILED.search(reply)
    elif kind == "btn":
        reply = device.command("BTN:" + rest)
        print(reply if reply else f"NO REPLY to BTN:{rest}")
        ok = reply == "BTN_OK" and wait_idle(device)
    elif kind == "state":
        reply, parsed = device.state()
        print(reply if reply else "NO REPLY to STATE")
        ok = parsed is not None
        if ok:
            fields.clear()
            fields.update(parsed)
    elif kind == "check":
        key, _, pattern = rest.partition("=")
        value = fields.get(key)
        ok = value is not None and re.fullmatch(pattern, value) is not None
        print(f"check {key}={value!r} ~ /{pattern}/: {'ok' if ok else 'FAILED'}")
    elif kind == "idle":
        ok = wait_idle(device, rest)
    elif kind == "shot":
        error = device.screenshot(rest, rotate)
        print(f"screenshot -> {rest}" if error is None else f"SCREENSHOT FAILED: {error}")
        ok = error is None
    elif kind == "wait":
        end = time.time() + float(rest)
        while device.read_line(end) is not None:
            pass
        ok = True
    elif kind == "expect":
        pattern, _, seconds = rest.rpartition(":")
        end = time.time() + float(seconds)
        found = None
        while (line := device.read_line(end)) is not None:
            if re.search(pattern, line):
                found = line
                break
        print(f"matched: {found}" if found else f"NOT SEEN: {pattern}")
        ok = found is not None
    else:
        print(f"unknown step {step!r}")
        ok = False
    return ok


def run(device, steps, rotate, keep_going=False):
    fields = {}
    all_ok = True
    for step in steps:
        try:
            ok = run_step(device, step, rotate, fields)
        except DeliveryUncertain as error:
            print(f"DELIVERY UNCERTAIN: {error}")
            ok = False
        if not ok:
            print(f"step failed: {step}")
            all_ok = False
            if not keep_going:
                return False
    return all_ok


def rotation(value):
    return value if value == "auto" else int(value)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyACM0", help="serial port (default /dev/ttyACM0)")
    ap.add_argument("--log", default=None, help="append the device's output here, timestamped")
    ap.add_argument("--rotate", type=rotation, default="auto",
                    help="degrees to turn screenshots (counter-clockwise), or auto (default): upright for "
                         "the orientation STATE reports")
    ap.add_argument("--script", default=None, help="read steps from this file, one per line")
    ap.add_argument("--keep-going", action="store_true", help="run every step even after one failed")
    ap.add_argument("steps", nargs="*")
    args = ap.parse_args()

    steps = list(args.steps)
    if args.script:
        with open(args.script, encoding="utf-8") as f:
            steps += [s.strip() for s in f if s.strip() and not s.lstrip().startswith("#")]
    if not steps:
        ap.error("no steps given")

    # Titles may be in any script: a console's code page must not stop the run.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    device = Device(args.port, args.log)
    try:
        ok = run(device, steps, args.rotate, args.keep_going)
    except ValueError as error:
        print(f"step failed: {error}")
        ok = False
    finally:
        # Whatever went wrong, the port closes RTS before DTR (see close()).
        device.close()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
