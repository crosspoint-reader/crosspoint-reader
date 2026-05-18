"""Serial control + observation harness for a connected X3/X4.

Opens the device's USB CDC port once, runs a reader thread that captures every
log line, exposes a small API for sending CMD: messages and waiting on
[STATE] / log signals. Built to drive ENABLE_SERIAL_LOG firmware; release/slim
builds will silently ignore button-inject commands.

Caveat for macOS: opening /dev/cu.usbmodem* with default DTR pulses the C3 USB
peripheral and triggers a chip reset. We force DTR/RTS off before any traffic.
"""
from __future__ import annotations

import glob
import platform
import re
import threading
import time
from collections import deque

import serial


PHYSICAL_BUTTONS = {"BACK", "CONFIRM", "LEFT", "RIGHT", "UP", "DOWN", "POWER"}


def _autodetect_port() -> str:
    system = platform.system()
    if system == "Darwin":
        # macOS exposes both /dev/cu.* and /dev/tty.* for the same USB CDC
        # device. Prefer /dev/cu.* (call-out): pyserial's DTR/RTS handling is
        # more reliable on cu.* than on tty.* under macOS.
        patterns = ["/dev/cu.usbmodem*", "/dev/tty.usbmodem*"]
    else:
        patterns = ["/dev/ttyACM*", "/dev/ttyUSB*"]
    for pat in patterns:
        candidates = sorted(glob.glob(pat))
        if candidates:
            return candidates[0]
    raise RuntimeError(
        f"no serial device found (searched {', '.join(repr(p) for p in patterns)}); "
        f"pass port=... explicitly"
    )


class Device:
    """One persistent serial connection that both observes logs and injects CMDs.

    Use as a context manager so the reader thread is torn down cleanly:

        with Device() as d:
            d.wait_for_activity("Home")
            d.tap("DOWN")
    """

    def __init__(self, port: str | None = None, baud: int = 115200) -> None:
        self.port = port or _autodetect_port()
        self.baud = baud
        self._ser: serial.Serial | None = None
        self._reader: threading.Thread | None = None
        self._stop = threading.Event()
        self._lines: deque[tuple[float, str]] = deque(maxlen=4096)
        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)
        # Absolute, monotonic count of lines appended over the session's
        # lifetime. Diverges from len(self._lines) once the deque rotates
        # past maxlen. Cursors (_send_cursor, since= args to wait_for) are
        # also absolute, so they stay correct after rotation. _slice_since()
        # translates absolute → deque-local for actual scanning.
        self._lines_total: int = 0
        # Absolute line index just before the most recent CMD send. wait_for()
        # defaults to scanning from here so common patterns like
        # `hold(); wait_for(predicate)` aren't racy against the firmware
        # processing the CMD faster than we re-enter the Condition.
        self._send_cursor: int = 0

    def __enter__(self) -> "Device":
        # Construct without a port so DTR/RTS are configured BEFORE open()
        # instead of pulsing high and then being lowered after. macOS's USB
        # CDC driver still appears to perturb the C3 on first enumeration
        # regardless, so this doesn't fully prevent the chip reset, but it
        # eliminates the pyserial-side contribution. The wait_for_activity
        # in go_home() and the resync logic in conftest's session fixture
        # both tolerate the reset; tests run through it cleanly.
        self._ser = serial.Serial()
        self._ser.port = self.port
        self._ser.baudrate = self.baud
        self._ser.timeout = 0.1
        self._ser.dtr = False
        self._ser.rts = False
        self._ser.open()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        return self

    def __exit__(self, *exc) -> None:
        self._stop.set()
        if self._reader is not None:
            self._reader.join(timeout=1.0)
        if self._ser is not None:
            self._ser.close()

    # ----- reader -----

    def _read_loop(self) -> None:
        assert self._ser is not None
        buf = b""
        while not self._stop.is_set():
            try:
                chunk = self._ser.read(256)
            except (OSError, serial.SerialException):
                time.sleep(0.1)
                continue
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").rstrip("\r")
                if not text:
                    continue
                with self._cond:
                    self._lines.append((time.time(), text))
                    self._lines_total += 1
                    self._cond.notify_all()

    # ----- inputs -----

    def _write(self, payload: str) -> None:
        """Send a CMD line. Snapshots the absolute line count first so the
        next wait_for() can scan lines emitted in response to this send, even
        if the firmware emits them before the host re-enters the Condition."""
        assert self._ser is not None
        with self._lock:
            self._send_cursor = self._lines_total
        self._ser.write(payload.encode("utf-8"))
        self._ser.flush()

    def press(self, button: str) -> None:
        button = button.upper()
        if button not in PHYSICAL_BUTTONS:
            raise ValueError(f"unknown physical button: {button}")
        self._write(f"CMD:BTN PRESS {button}\n")

    def release(self, button: str) -> None:
        button = button.upper()
        if button not in PHYSICAL_BUTTONS:
            raise ValueError(f"unknown physical button: {button}")
        self._write(f"CMD:BTN RELEASE {button}\n")

    def tap(self, button: str, hold_ms: int = 80) -> None:
        """Server-side tap: firmware schedules its own auto-release after
        hold_ms. Avoids round-trip latency on the release; the activity layer's
        wasReleased() edge still fires normally."""
        button = button.upper()
        if button not in PHYSICAL_BUTTONS:
            raise ValueError(f"unknown physical button: {button}")
        self._write(f"CMD:BTN TAP {button}\n" if hold_ms == 80
                    else f"CMD:BTN HOLD {button} {hold_ms}\n")

    def hold(self, button: str, ms: int) -> None:
        """Press and hold for `ms`, then auto-release. Same primitive as tap,
        named for readability when hold time is the point (e.g. power button
        long-press, settle windows)."""
        button = button.upper()
        if button not in PHYSICAL_BUTTONS:
            raise ValueError(f"unknown physical button: {button}")
        self._write(f"CMD:BTN HOLD {button} {ms}\n")

    def request_state(self) -> None:
        """Ask the firmware to re-emit the current [STATE] line. Useful after
        opening the connection to learn what activity we're on without waiting
        for the next transition."""
        self._write("CMD:STATE\n")

    def go_home(self, timeout: float = 10.0) -> None:
        """Test-harness escape: jump straight to Home regardless of current
        state. The firmware calls activityManager.goHome() so this works from
        any activity (Reader, FileBrowser, deep settings, etc.) without
        navigation guessing. Use at the start of each test for a clean
        baseline."""
        if self.current_activity() == "Home":
            return
        self._write("CMD:HOME\n")
        # _write snapshots _send_cursor; wait_for defaults to scanning from
        # that point so the [STATE] line emitted while we re-enter the
        # Condition is still seen.
        self.wait_for_activity("Home", timeout=timeout)

    def home_menu(self, timeout: float = 3.0) -> tuple[list[str], int]:
        """Return (items, selector) from the [HOME] menu= line emitted by the
        current Home session. Symbols are stable (FileBrowser, Recents,
        OpdsBrowser if configured, FileTransfer, Settings, plus RecentBook<i>
        entries for each recent book).

        Bounded to lines from after the most recent CMD (_send_cursor) so we
        don't pick up a stale selector value from a previous Home entry; the
        wait timeout covers the ~100ms gap between [STATE] activity=Home and
        Home's first render."""
        with self._lock:
            window = self._slice_since(self._send_cursor)
        for _ts, text in reversed(window):
            m = self._HOME_MENU_RE.search(text)
            if m:
                return m.group(1).split(","), int(m.group(2))
        # No [HOME] line emitted since the last CMD; wait for the next one.
        line = self.wait_for(
            lambda text: bool(self._HOME_MENU_RE.search(text)),
            timeout=timeout,
        )
        m = self._HOME_MENU_RE.search(line)
        assert m is not None
        return m.group(1).split(","), int(m.group(2))

    def navigate_home_to(self, target: str) -> None:
        """Tap Down/Up to reach the named menu item on Home, then Confirm.
        Reads the current menu from the [HOME] log so it adapts to the
        actual device config (OPDS configured, recent books present, etc.).
        Each step is bounded by wait_for_refresh's 3s timeout, so the worst
        case (full 7-item walk + Confirm) is under ~25s.

        Raises ValueError if target isn't a valid menu item on this device.
        """
        items, selector = self.home_menu()
        if target not in items:
            raise ValueError(
                f"{target!r} not in Home menu {items!r} on this device"
            )
        target_idx = items.index(target)
        delta = target_idx - selector
        button = "DOWN" if delta > 0 else "UP"
        for _ in range(abs(delta)):
            self.tap(button)
            self.wait_for_refresh(timeout=3.0)
        self.tap("CONFIRM")

    # ----- observation -----

    def lines_count(self) -> int:
        """Absolute count of lines captured over the session lifetime. Pair
        with wait_for's `since=` to wait for events produced after a specific
        moment (e.g. after sending a CMD). Survives deque rotation: returns a
        monotonically-increasing value even after older lines age out."""
        with self._lock:
            return self._lines_total

    def _slice_since(self, since: int) -> list[tuple[float, str]]:
        """Return _lines from absolute index `since` onwards. Caller must
        hold _lock. Translates the absolute index to a deque-local one,
        accounting for any lines that have aged out of the maxlen window."""
        base = self._lines_total - len(self._lines)
        local_start = max(0, since - base)
        return list(self._lines)[local_start:]

    def wait_for(self, predicate, timeout: float = 5.0, since: int | None = None) -> str:
        """Block until a captured log line satisfies predicate. Returns the
        matching line. Raises TimeoutError otherwise.

        `since` is an absolute index from lines_count(). If omitted, defaults
        to the snapshot taken just before the most recent CMD send, so common
        patterns like `hold(...); wait_for(...)` aren't racy. Pass
        since=lines_count() explicitly to scan only future lines, or since=0
        to scan everything still in the buffer."""
        deadline = time.time() + timeout
        with self._cond:
            start = since if since is not None else self._send_cursor
            while True:
                for _ts, text in self._slice_since(start):
                    if predicate(text):
                        return text
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise TimeoutError(f"wait_for timed out after {timeout:.1f}s")
                self._cond.wait(timeout=remaining)

    _STATE_RE = re.compile(r"\[STATE\]\s+activity=(\S*)")
    _REFRESH_RE = re.compile(r"\[REFRESH\]\s+done")
    _HOME_MENU_RE = re.compile(r"\[HOME\]\s+menu=(\S+)\s+selector=(\d+)")

    def wait_for_activity(self, name: str, timeout: float = 10.0, since: int | None = None) -> None:
        def matches(text: str) -> bool:
            m = self._STATE_RE.search(text)
            return bool(m and m.group(1) == name)
        self.wait_for(matches, timeout=timeout, since=since)

    def wait_for_refresh(self, timeout: float = 5.0) -> None:
        """Block until the next [REFRESH] done line emitted by the render task.
        Use after a tap that should produce a visible change, before asserting
        on the next state."""
        self.wait_for(lambda text: bool(self._REFRESH_RE.search(text)), timeout=timeout)

    def current_activity(self) -> str | None:
        """Most-recent [STATE] activity, if any. Returns None if no [STATE] has
        been seen yet; call request_state() to force one."""
        with self._lock:
            for _ts, text in reversed(self._lines):
                m = self._STATE_RE.search(text)
                if m:
                    return m.group(1) or None
        return None
