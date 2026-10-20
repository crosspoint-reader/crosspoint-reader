#!/usr/bin/env python3
"""Drive the reader over USB serial with the serial test API (docs/serial-test-api.md).

    tools/devctl.py [--port P] [--log FILE] [--rotate DEG] STEP...
    tools/devctl.py [--port P] --script steps.txt

Steps run in order; the run stops at the first failed step and exits non-zero (--keep-going runs
them all and exits non-zero if any failed).

  cmd:<TEXT>            send "CMD:<TEXT>", wait for its reply line, print it
                        (fails on a *_ERROR / *_TIMEOUT reply or none at all)
  state                 CMD:STATE, print the reply and remember its fields for `check:`
  check:<key>=<regex>   the last STATE's <key> must match <regex> (re.fullmatch)
  idle[:<ms>[:SCREEN]]  CMD:IDLE..., wait for IDLE / IDLE_TIMEOUT (a timeout fails the step)
  shot:<file.png>       CMD:SCREENSHOT, decode the 1-bit frame, rotate it, save it
  wait:<seconds>        read (and log) the device's output for a while
  expect:<regex>:<sec>  read until an output line matches (re.search), fail otherwise
  select:<regex>[:<n>]  press Down, waiting for each, until STATE's label matches (re.fullmatch); at most n
                        presses (default 40). The label is checked first, so a row already selected takes none.

A --script file holds one step per line; blank lines and lines starting with # are skipped.
Every line the device prints goes to --log with a timestamp. Needs pyserial, and Pillow for shot:.

The port is opened with the default DTR/RTS lines, which does not reset an ESP32-C3 (unlike
tools/serial_transfer.py, which deasserts them), and closed with RTS dropped before DTR, which does
not either (a plain close on Windows does). The device re-enumerates its USB port across a
reboot or a sleep; the reader reopens it and carries on.
"""
import argparse
import os
import re
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")

# First words of every reply the firmware sends to a command (see docs/serial-test-api.md).
REPLY = re.compile(
    r"^(BTN_|ACT_|STATE:|CMD_ERROR|GET:|GET_END|GET_ERROR|SET_|LOG_|TOUCH_|SWIPE_|SLEEP_OK|REBOOT_OK|"
    r"EVENTS_|IDLE|DICT|HELP:|BOOTLOG_|HEAP_|TTF:|TTF_|FS:|FS_)"
)
# Commands that answer with several lines, and the line that ends them.
MULTI_LINE = {"GET:*": re.compile(r"^GET_END"), "FS:LS:": re.compile(r"^FS_END")}
FAILED = re.compile(r"_ERROR|_TIMEOUT|^CMD_ERROR")


def native_path(path):
    """A Git Bash path (/c/tmp/x.png) as Windows' Python opens it (C:/tmp/x.png); others as they are."""
    match = re.match(r"^/([a-zA-Z])/(.*)$", path)
    return f"{match.group(1).upper()}:/{match.group(2)}" if os.name == "nt" and match else path


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


class Device:
    def __init__(self, port, log_path=None):
        self.port = port
        self.log = open(log_path, "a", encoding="utf-8", errors="replace") if log_path else None
        self.t0 = time.time()
        self.buf = b""
        self.ser = self._open()

    def note(self, line):
        if self.log:
            self.log.write(f"[{time.time() - self.t0:8.2f}] {line}\n")
            self.log.flush()

    def _open(self):
        for _ in range(60):
            try:
                s = serial.Serial(self.port, 115200, timeout=0.2)
                s.write(b"\r\n")  # C3 USB-CDC output waits until the host has written something
                return s
            except (serial.SerialException, OSError):
                time.sleep(0.5)
        sys.exit(f"cannot open {self.port}")

    def close(self):
        # RTS goes low before DTR: a plain close on Windows drops DTR first, and the ESP32-C3's USB serial unit
        # takes RTS high with DTR low as a reset request (esptool's), so every run restarted the device to Home.
        if self.ser.is_open:
            self.ser.rts = False
            self.ser.dtr = False
        self.ser.close()

    def _read(self, size):
        try:
            return self.ser.read(size)
        except (serial.SerialException, OSError):
            # The port went away (reboot, sleep, re-enumeration): reopen and go on.
            try:
                self.close()
            except Exception:
                pass
            time.sleep(1)
            self.ser = self._open()
            return b""

    def read_line(self, deadline):
        while time.time() < deadline:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line, self.buf = self.buf[:nl], self.buf[nl + 1:]
                text = line.decode("utf-8", errors="replace").rstrip("\r")
                self.note(text)
                return text
            self.buf += self._read(4096)
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

    def send(self, text):
        self.drain()
        self.note(f">>> CMD:{text}")
        self.ser.write(("CMD:" + text + "\n").encode())

    def command(self, text, timeout=20, until=None, lines=None):
        """Send a command; return its reply line (the terminator of a several-line reply), or None. With `until`,
        the lines before the terminator that are not log lines go to `lines`."""
        self.send(text)
        deadline = time.time() + timeout
        while (line := self.read_line(deadline)) is not None:
            if until is not None:
                if until.match(line) or FAILED.search(line):
                    return line
                if lines is not None and REPLY.match(line):
                    lines.append(line)
                continue
            if REPLY.match(line):
                return line
        return None

    def screenshot(self, path, rotate):
        self.send("SCREENSHOT")
        deadline = time.time() + 30
        while (line := self.read_line(deadline)) is not None:
            if line.startswith("SCREENSHOT_START:"):
                _, w, h, size = line.split(":")
                data = self.read_exact(int(size), time.time() + 30)
                if len(data) != int(size):
                    return f"short frame: {len(data)} of {size} bytes"
                from PIL import Image  # only shot: needs Pillow

                image = Image.frombytes("1", (int(w), int(h)), data)
                if rotate:
                    image = image.rotate(rotate, expand=True)
                image.save(path)
                end = self.read_line(time.time() + 5)
                return None if end == "SCREENSHOT_END" else f"no SCREENSHOT_END (got {end!r})"
            if line.startswith("SCREENSHOT_ERROR"):
                return line
        return "no reply"


def idle_timeout(arg):
    """Seconds to wait for the reply to CMD:IDLE<arg>: the device's own timeout and some slack."""
    first = arg.split(":")[0] if arg else ""
    return (int(first) if first.isdigit() else 10000) / 1000 + 5


def run(device, steps, rotate, keep_going=False):
    state = {}
    all_ok = True
    for step in steps:
        kind, _, rest = step.partition(":")
        ok = True
        if kind == "cmd":
            until = next((end for start, end in MULTI_LINE.items() if rest.startswith(start)), None)
            timeout = idle_timeout(rest[5:]) if rest.startswith("IDLE") else 20
            lines = []
            reply = device.command(rest, timeout=timeout, until=until, lines=lines)
            for line in lines:
                print(line)
            print(reply if reply else f"NO REPLY to {rest}")
            ok = bool(reply) and not FAILED.search(reply)
        elif kind == "state":
            reply = device.command("STATE")
            print(reply if reply else "NO REPLY to STATE")
            ok = bool(reply) and reply.startswith("STATE:")
            if ok:
                state = split_fields(reply[len("STATE:"):])
        elif kind == "check":
            key, _, pattern = rest.partition("=")
            value = state.get(key)
            ok = value is not None and re.fullmatch(pattern, value) is not None
            print(f"check {key}={value!r} ~ /{pattern}/: {'ok' if ok else 'FAILED'}")
        elif kind == "idle":
            reply = device.command("IDLE" + (":" + rest if rest else ""), timeout=idle_timeout(rest))
            print(reply if reply else "NO REPLY to IDLE")
            ok = bool(reply) and reply.startswith("IDLE:")
        elif kind == "shot":
            path = native_path(rest)
            error = device.screenshot(path, rotate)
            print(f"screenshot -> {path}" if error is None else f"SCREENSHOT FAILED: {error}")
            ok = error is None
        elif kind == "select":
            pattern, _, most = rest.partition(":")
            presses = int(most) if most.isdigit() else 40
            ok = False
            for press in range(presses + 1):
                reply = device.command("STATE")
                fields = split_fields(reply[len("STATE:"):]) if reply and reply.startswith("STATE:") else {}
                if re.fullmatch(pattern, fields.get("label", "")):
                    state = fields
                    ok = True
                    print(f"selected {fields.get('label')!r} after {press} presses")
                    break
                if press == presses:
                    break
                # One press at a time: presses sent back to back reach the list faster than it redraws, and it
                # moves further than asked.
                device.command("BTN:DOWN")
                device.command("IDLE:5000", timeout=idle_timeout("5000"))
            if not ok:
                print(f"NOT SELECTED: {pattern} (last label {fields.get('label')!r})")
        elif kind == "wait":
            end = time.time() + float(rest)
            while device.read_line(end) is not None:
                pass
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
        if not ok:
            print(f"step failed: {step}")
            all_ok = False
            if not keep_going:
                return False
    return all_ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--log", default=None, help="append the device's output here, timestamped")
    ap.add_argument("--rotate", type=int, default=-90,
                    help="rotate screenshots by this many degrees (default -90: the X4's 800x480 frame "
                         "upright in portrait; 0 keeps the panel's native frame)")
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

    # Labels, chapter titles and popups may be in any script: a console's code page must not stop the run.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    device = Device(args.port, native_path(args.log) if args.log else None)
    try:
        ok = run(device, steps, args.rotate, args.keep_going)
    finally:
        # Whatever went wrong, the port closes RTS before DTR: an exception that closed it plainly would reset the
        # device (see close()).
        device.close()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
