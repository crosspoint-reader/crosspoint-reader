"""scripts/devctl.py against a scripted port: reconnects, reply matching and reply timeouts."""

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import time
import types
import unittest
from unittest.mock import patch


class SerialException(OSError):
    pass


# devctl uses only these names of pyserial, which the test runners need not have. The stand-in is in
# sys.modules only while devctl is loaded; devctl keeps its own reference to it.
serial = types.ModuleType("serial")
serial.SerialException = SerialException
_previous_serial = sys.modules.get("serial")
sys.modules["serial"] = serial
try:
    spec = importlib.util.spec_from_file_location(
        "devctl", Path(__file__).resolve().parents[1] / "scripts/devctl.py"
    )
    devctl = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(devctl)
finally:
    if _previous_serial is None:
        del sys.modules["serial"]
    else:
        sys.modules["serial"] = _previous_serial


class FakePort:
    """Bytes arrive at their scheduled time. `replies` maps a written line to (bytes, delay)."""

    def __init__(self, replies=None, fail_writes=0, fail_reads=0):
        self.replies = replies or {}
        self.fail_writes = fail_writes  # commands whose write raises, as on a port that went away
        self.fail_reads = fail_reads  # reads after a command that raise, likewise
        self.incoming = []  # (time, bytes)
        self.written = []
        self.is_open = True
        self.rts = self.dtr = True

    def arrive(self, data, after=0.0):
        self.incoming.append((time.time() + after, data))

    def _due(self):
        now = time.time()
        data = b"".join(data for at, data in self.incoming if at <= now)
        self.incoming = [(at, data) for at, data in self.incoming if at > now]
        return now, data

    @property
    def in_waiting(self):
        return sum(len(data) for at, data in self.incoming if at <= time.time())

    def reset_input_buffer(self):
        self._due()

    def read(self, size):
        if self.fail_reads and len(self.written) > 1:
            self.fail_reads -= 1
            raise SerialException("read failed: device reports readiness to read but returned no data")
        now, data = self._due()
        if len(data) > size:
            self.incoming.insert(0, (now, data[size:]))
        elif not data:
            time.sleep(0.005)
        return data[:size]

    def write(self, data):
        if data != b"\r\n" and self.fail_writes:
            self.fail_writes -= 1
            raise SerialException("write failed: [Errno 5] Input/output error")
        self.written.append(data)
        if data in self.replies:
            self.arrive(*self.replies[data])
        return len(data)

    def close(self):
        self.is_open = False


def open_device(*ports):
    queue = list(ports)
    serial.Serial = lambda *args, **kwargs: queue.pop(0)
    return devctl.Device("/dev/fake")


def run(device, steps):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        ok = devctl.run(device, steps, rotate=0, keep_going=True)
    return ok, out.getvalue()


class DevctlTest(unittest.TestCase):
    def test_a_failed_write_reopens_the_port_and_is_not_sent_again(self):
        old = FakePort(fail_writes=1)
        new = FakePort(replies={b"CMD:HELP\n": (b"HELP: BTN ACT STATE IDLE SCREENSHOT HELP\n", 0)})
        device = open_device(old, new)
        ok, out = run(device, ["cmd:ACT:HOME", "cmd:HELP"])
        self.assertFalse(ok)
        self.assertIn("DELIVERY UNCERTAIN: CMD:ACT:HOME", out)
        self.assertIn("HELP: BTN", out)
        self.assertEqual(new.written, [b"\r\n", b"CMD:HELP\n"], "ACT:HOME is not sent again")

    @patch.object(devctl, "REPLY_TIMEOUT_S", 0.5)
    def test_a_port_lost_while_waiting_fails_the_step_and_the_run_goes_on(self):
        old = FakePort(fail_reads=1)
        new = FakePort(replies={b"CMD:HELP\n": (b"HELP: BTN ACT STATE IDLE SCREENSHOT HELP\n", 0)})
        device = open_device(old, new)
        ok, out = run(device, ["state", "cmd:HELP"])
        self.assertFalse(ok)
        self.assertIn("NO REPLY to STATE: the port was reopened", out)
        self.assertIn("HELP: BTN", out)
        self.assertNotIn("OUT OF STEP", out)

    def test_a_reply_to_another_command_is_skipped(self):
        late = b"IDLE:1234\nI (500) a log line\n"
        port = FakePort(replies={b"CMD:ACT:HOME\n": (late + b"ACT_OK\n", 0)})
        device = open_device(port)
        self.assertEqual(device.command("ACT:HOME"), "ACT_OK")

    def test_cmd_error_answers_any_command(self):
        port = FakePort(replies={b"CMD:NOPE\n": (b"CMD_ERROR:unknown command NOPE\n", 0)})
        device = open_device(port)
        self.assertEqual(device.command("NOPE"), "CMD_ERROR:unknown command NOPE")

    def test_a_command_with_spaces_around_it_gets_its_reply(self):
        port = FakePort(replies={b"CMD: STATE \n": (b"STATE: activity=Home\n", 0)})
        device = open_device(port)
        self.assertEqual(device.command(" STATE "), "STATE: activity=Home")

    # The reply comes, but late: long after any quiet wait. It must not pass for the next STATE's.
    @patch.object(devctl, "REPLY_TIMEOUT_S", 0.2)
    def test_a_reply_timeout_stops_the_run_even_with_keep_going(self):
        port = FakePort()
        device = open_device(port)
        port.arrive(b"STATE: activity=Old\n", after=1.5)
        ok, out = run(device, ["state", "state"])
        self.assertFalse(ok)
        self.assertIn("OUT OF STEP: no reply to CMD:STATE within 0.2 s", out)
        self.assertEqual(port.written, [b"\r\n", b"CMD:STATE\n"], "the second STATE is not sent")

    def test_a_busy_idle_stops_the_run(self):
        port = FakePort(replies={b"CMD:IDLE\n": (b"IDLE_ERROR:busy\n", 0)})
        device = open_device(port)
        ok, out = run(device, ["idle", "state"])
        self.assertFalse(ok)
        self.assertIn("OUT OF STEP: CMD:IDLE answered IDLE_ERROR:busy", out)
        self.assertEqual(port.written, [b"\r\n", b"CMD:IDLE\n"])

    @patch.object(devctl, "FRAME_STALL_S", 0.2)
    def test_a_frame_cut_short_stops_the_run(self):
        frame = (b"SCREENSHOT_START:100:8:100\n" + bytes(40), 0)
        port = FakePort(replies={b"CMD:SCREENSHOT\n": frame})
        device = open_device(port)
        ok, out = run(device, ["shot:unused.png", "state"])
        self.assertFalse(ok)
        self.assertIn("OUT OF STEP: the frame stopped after 40 of 100 bytes", out)
        self.assertEqual(port.written, [b"\r\n", b"CMD:SCREENSHOT\n"])

    def test_the_idle_reply_timeout_follows_the_device_timeout(self):
        self.assertEqual(devctl.reply_timeout("IDLE"), 15)
        self.assertEqual(devctl.reply_timeout("IDLE:30000"), 35)
        self.assertEqual(devctl.reply_timeout(" IDLE:30000 "), 35)
        self.assertEqual(devctl.reply_timeout("IDLE:+30000"), 35)
        self.assertEqual(devctl.reply_timeout("IDLE:900000"), 605, "the device caps it at 600 s")
        self.assertEqual(devctl.reply_timeout("STATE"), 20)

    # A step's text goes to the device as written, so the wait must come from that text: a
    # `cmd: IDLE:300` step waits the device's 300 ms and the margin, not the 20 ms of other commands.
    @patch.object(devctl, "REPLY_TIMEOUT_S", 0.02)
    @patch.object(devctl, "IDLE_MARGIN_S", 0.1)
    def test_a_cmd_idle_step_waits_as_long_as_the_device(self):
        port = FakePort()
        device = open_device(port)
        start = time.time()
        ok, out = run(device, ["cmd: IDLE:300"])
        self.assertFalse(ok)
        self.assertIn("OUT OF STEP: no reply to CMD: IDLE:300 within 0.4 s", out)
        self.assertGreaterEqual(time.time() - start, 0.4)


if __name__ == "__main__":
    unittest.main()
