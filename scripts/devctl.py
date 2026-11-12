#!/usr/bin/env python3
"""Drive the reader over USB serial with the serial test API (docs/contributing/serial-test-api.md).

    scripts/devctl.py [--port P] [--log FILE] [--rotate DEG|auto] STEP...
    scripts/devctl.py [--port P] --script steps.txt

Steps run in order; the run stops at the first failed step and exits non-zero (--keep-going runs
them all and exits non-zero if any failed). Either way it stops when it is OUT OF STEP (below).

  cmd:<TEXT>            send "CMD:<TEXT>", wait for its reply line, print it
                        (fails on a *_ERROR / *_TIMEOUT reply)
  btn:<NAME>[:<how>]    CMD:BTN:<NAME>[:<how>], then wait until the device is idle (CMD:IDLE with
                        the default 10 s plus the hold)
  state                 CMD:STATE, print the reply and remember its fields for `check:`
  check:<key>=<regex>   the last STATE's <key> must match <regex> (re.fullmatch)
  idle[:<ms>]           CMD:IDLE[:<ms>], wait for its answer (IDLE_TIMEOUT or IDLE_ERROR fails it)
  shot:<file.png>       CMD:SCREENSHOT, decode the 1-bit frame, rotate it, save it (needs Pillow)
  wait:<seconds>        read (and log) the device's output for a while
  expect:<regex>:<sec>  read until an output line matches (re.search), fail otherwise

A --script file holds one step per line; blank lines and lines starting with # are skipped.
Every line the device prints goes to --log with a timestamp. A step that cannot be run as written
(a pattern or number that does not parse, a command too long to send) fails like any other.

A step takes only a reply to its own command (its name and '_' or ':', or a CMD_ERROR) and waits for
it as long as the device may take: an IDLE its own timeout and 5 s, a screenshot 30 s to start,
then until no part of the frame has come for 20 s, then 20 s for its end; any other command 20 s.
A reply that has not come by then may still come, and nothing would tell it from the reply to the
next command of its name, so the run stops there, OUT OF STEP, even with --keep-going. So it does
after IDLE_ERROR:busy: a wait started earlier will still answer.

The port is opened with the default DTR/RTS lines, which does not reset the reader, and closed with
RTS dropped before DTR: RTS high with DTR low is esptool's reset request, which a plain close on
Windows can send. When the port goes away (the cable is pulled, the device sleeps), the tool
reopens it and drops what had come in. A step whose command was being written then fails as
DELIVERY UNCERTAIN and is not sent again (the device may have run it); a step waiting for its reply
fails with NO REPLY. With --keep-going the run goes on from there. On the ESP32-C3 a restart or a
reset keeps the port: a reply outstanding across it never comes, and the run stops OUT OF STEP.

Replies carry no tag. Stopping on a missing reply keeps one run in step, but a reply still on its
way when a run starts (from an IDLE a stopped run left waiting) can be taken by the first command
of its name, if it arrives just as that command is sent.
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
# How long to wait for a reply. The device answers a command in the loop pass that reads it, and
# CMD:IDLE when its wait ends (IDLE_DEFAULT_MS, at most IDLE_MAX_MS); it gives up on a frame the
# host has not taken for 15 s.
REPLY_TIMEOUT_S = 20
IDLE_DEFAULT_MS = 10000
IDLE_MAX_MS = 600000
IDLE_MARGIN_S = 5
SCREENSHOT_START_S = 30
FRAME_STALL_S = 20
# The holds of CMD:BTN:<name> and :LONG (ButtonPressQueue::SHORT_HOLD_MS, LONG_HOLD_MS).
SHORT_HOLD_MS = 60
LONG_HOLD_MS = 1100

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


def reply_timeout(text):
    """Seconds to wait for the reply to CMD:<text>: as long as the device may take to send it."""
    name, _, arg = text.strip(" \t").partition(":")
    if name != "IDLE":
        return REPLY_TIMEOUT_S
    try:
        ms = int(arg) if arg else IDLE_DEFAULT_MS
    except ValueError:
        ms = IDLE_DEFAULT_MS  # the device refuses it at once
    return max(0, min(ms, IDLE_MAX_MS)) / 1000 + IDLE_MARGIN_S


class DeliveryUncertain(Exception):
    """Writing a command failed with the port: the device may or may not have received it."""


class OutOfStep(Exception):
    """A reply may still come that nothing would tell from the reply to a next command: stop."""


class Device:
    def __init__(self, port, log_path=None):
        self.port = port
        self.log = open(log_path, "a", encoding="utf-8", errors="replace") if log_path else None
        self.t0 = time.time()
        self.buf = b""
        self.reopens = 0  # replies to commands sent before a reopen went with the old connection
        self.ser = self._open()

    def note(self, line):
        if self.log:
            self.log.write(f"[{time.time() - self.t0:8.2f}] {line}\n")
            self.log.flush()

    def _open(self):
        for _ in range(60):
            try:
                s = serial.Serial(self.port, 115200, timeout=0.2)
                s.reset_input_buffer()
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
        try:
            self.ser.close()
        except (serial.SerialException, OSError):
            pass  # the port went away

    def _reopen(self):
        """The port went away (unplugged, sleep, re-enumeration): reopen it, without what had come in."""
        self.close()
        time.sleep(1)
        self.buf = b""
        self.reopens += 1
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

    def read_exact(self, size, stall_s):
        """`size` bytes, or fewer when none have come for `stall_s` seconds."""
        last = time.time()
        while len(self.buf) < size and time.time() - last < stall_s:
            have = len(self.buf)
            self.buf += self._read(min(65536, size - len(self.buf)))
            if len(self.buf) > have:
                last = time.time()
        data, self.buf = self.buf[:size], self.buf[size:]
        return data

    def drain(self):
        """Log and drop the lines already received (the log). A partial line is kept."""
        try:
            waiting = self.ser.in_waiting
        except (serial.SerialException, OSError):
            waiting = 0
        if waiting:
            self.buf += self._read(waiting)
        while (nl := self.buf.find(b"\n")) >= 0:
            line, self.buf = self.buf[:nl], self.buf[nl + 1:]
            self.note(line.decode("utf-8", errors="replace").rstrip("\r"))

    def send(self, text):
        line = ("CMD:" + text).encode()
        if len(line) > MAX_LINE:
            raise ValueError(f"command longer than {MAX_LINE} bytes: {text[:40]}...")
        self.drain()
        self.note(f">>> CMD:{text}")
        try:
            self.ser.write(line + b"\n")
        except (serial.SerialException, OSError) as error:
            self.note(f"write failed: {error}")
            self._reopen()
            raise DeliveryUncertain(f"CMD:{text} (port reopened, not resent)") from error

    def command(self, text):
        """Send a command and return its reply line. Other lines are skipped: log lines and replies to
        commands of another name. None when the port was reopened meanwhile: the reply went with the
        old connection. OutOfStep when no reply came in time, or after IDLE_ERROR:busy."""
        family = reply_family(text)
        timeout = reply_timeout(text)
        reopens = self.reopens
        self.send(text)
        deadline = time.time() + timeout
        while self.reopens == reopens and (line := self.read_line(deadline)) is not None:
            if not is_reply_to(line, family):
                continue
            if line == "IDLE_ERROR:busy":
                raise OutOfStep(f"CMD:{text} answered {line}: a wait started earlier will still answer")
            return line
        if self.reopens != reopens:
            return None
        raise OutOfStep(f"no reply to CMD:{text} within {timeout:g} s")

    def state(self):
        reply = self.command("STATE")
        fields = split_fields(reply[len("STATE:"):]) if reply and reply.startswith("STATE:") else None
        return reply, fields

    def screenshot(self, path, rotate):
        if rotate == "auto":
            _, fields = self.state()
            rotate = ROTATION.get((fields or {}).get("orientation", ""), -90)
        reopens = self.reopens
        self.send("SCREENSHOT")
        deadline = time.time() + SCREENSHOT_START_S
        while self.reopens == reopens and (line := self.read_line(deadline)) is not None:
            if line.startswith("SCREENSHOT_START:"):
                parts = line.split(":")
                if not parts[1].isdigit():
                    raise OutOfStep(f"{line!r} gives no frame size, so the frame cannot be skipped")
                size = int(parts[1])
                data = self.read_exact(size, FRAME_STALL_S)
                if self.reopens != reopens:
                    return "the port was reopened during the frame"
                if len(data) != size:
                    raise OutOfStep(f"the frame stopped after {len(data)} of {size} bytes")
                end_deadline = time.time() + REPLY_TIMEOUT_S
                end = self.read_line(end_deadline)
                while end is not None and self.reopens == reopens and not end.startswith("SCREENSHOT_"):
                    end = self.read_line(end_deadline)
                if self.reopens != reopens:
                    return "the port was reopened after the frame"
                if end is None:
                    raise OutOfStep(f"no SCREENSHOT_END within {REPLY_TIMEOUT_S} s of the frame")
                if end != "SCREENSHOT_END":
                    return end
                if len(parts) >= 4:
                    width, height = int(parts[2]), int(parts[3])
                elif size in PANELS:
                    width, height = PANELS[size]
                else:
                    return f"unknown panel for a {size}-byte frame"
                from PIL import Image  # only shot: needs Pillow

                image = Image.frombytes("1", (width, height), data)
                if rotate:
                    image = image.rotate(rotate, expand=True)
                image.save(path)
                return None
            if line.startswith("SCREENSHOT_ERROR") or line.startswith("CMD_ERROR"):
                return line
        if self.reopens != reopens:
            return "no reply: the port was reopened"
        raise OutOfStep(f"no reply to CMD:SCREENSHOT within {SCREENSHOT_START_S} s")


def no_reply(text):
    return f"NO REPLY to {text}: the port was reopened"


def press_ms(arg):
    """How long CMD:BTN:<arg> holds its button, as the device plays it (ButtonPressQueue)."""
    how = arg.strip(" \t").partition(":")[2]
    if how == "LONG":
        return LONG_HOLD_MS
    if how == "DOUBLE":
        return 2 * SHORT_HOLD_MS
    return int(how) if how.isdigit() else SHORT_HOLD_MS


def wait_idle(device, arg=""):
    text = "IDLE" + (":" + arg if arg else "")
    reply = device.command(text)
    print(reply if reply else no_reply(text))
    return bool(reply) and reply.startswith("IDLE:")


def run_step(device, step, rotate, fields):
    """Runs one step and returns whether it passed. `fields` keeps the last STATE's fields."""
    kind, _, rest = step.partition(":")
    if kind == "cmd":
        reply = device.command(rest)
        print(reply if reply else no_reply(rest))
        ok = bool(reply) and not FAILED.search(reply)
    elif kind == "btn":
        reply = device.command("BTN:" + rest)
        print(reply if reply else no_reply("BTN:" + rest))
        # The device's default wait would run out under a long hold: the hold comes on top.
        ok = reply == "BTN_OK" and wait_idle(device, str(IDLE_DEFAULT_MS + press_ms(rest)))
    elif kind == "state":
        reply, parsed = device.state()
        print(reply if reply else no_reply("STATE"))
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
        except (ValueError, re.error, ImportError) as error:
            # A bad step (a number or pattern that does not parse, a command too long, Pillow
            # missing) or a frame of the wrong size, found with the link still in step.
            print(f"{type(error).__name__}: {error}")
            ok = False
        except OutOfStep as error:
            print(f"OUT OF STEP: {error}; a reply may still come, so the run stops")
            print(f"step failed: {step}")
            return False
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
    finally:
        # Whatever went wrong, the port closes RTS before DTR (see close()).
        device.close()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
