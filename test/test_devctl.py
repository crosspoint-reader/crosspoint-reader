"""scripts/devctl.py against a scripted port: reconnects, reply matching and resync."""

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


# devctl uses only these names of pyserial, which the test runners need not have.
serial = types.ModuleType("serial")
serial.SerialException = SerialException
sys.modules["serial"] = serial

spec = importlib.util.spec_from_file_location(
    "devctl", Path(__file__).resolve().parents[1] / "scripts/devctl.py"
)
devctl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(devctl)


class FakePort:
    """Bytes arrive at their scheduled time. `replies` maps a written line to (bytes, delay)."""

    def __init__(self, replies=None, fail_writes=0):
        self.replies = replies or {}
        self.fail_writes = fail_writes  # commands whose write raises, as on a port that went away
        self.incoming = []  # (time, bytes)
        self.written = []
        self.is_open = True
        self.rts = self.dtr = True

    def arrive(self, data, after=0.0):
        self.incoming.append((time.time() + after, data))

    @property
    def in_waiting(self):
        return sum(len(data) for at, data in self.incoming if at <= time.time())

    def read(self, size):
        now = time.time()
        data = b"".join(data for at, data in self.incoming if at <= now)
        self.incoming = [(at, data) for at, data in self.incoming if at > now]
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


@patch.object(devctl, "RESYNC_QUIET_S", 0.3)
class DevctlTest(unittest.TestCase):
    def test_a_failed_write_reopens_the_port_and_is_not_sent_again(self):
        old = FakePort(fail_writes=1)
        new = FakePort(replies={b"CMD:HELP\n": (b"HELP: BTN ACT STATE IDLE SCREENSHOT HELP\n", 0)})
        device = open_device(old, new)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            ok = devctl.run(device, ["cmd:ACT:HOME", "cmd:HELP"], rotate=0, keep_going=True)
        self.assertFalse(ok)
        self.assertIn("DELIVERY UNCERTAIN: CMD:ACT:HOME", out.getvalue())
        self.assertIn("HELP: BTN", out.getvalue())
        self.assertEqual(new.written, [b"\r\n", b"CMD:HELP\n"], "ACT:HOME is not sent again")

    def test_a_reply_to_another_command_is_skipped(self):
        late = b"IDLE:1234\nI (500) a log line\n"
        port = FakePort(replies={b"CMD:ACT:HOME\n": (late + b"ACT_OK\n", 0)})
        device = open_device(port)
        self.assertEqual(device.command("ACT:HOME", timeout=2), "ACT_OK")

    def test_cmd_error_answers_any_command(self):
        port = FakePort(replies={b"CMD:NOPE\n": (b"CMD_ERROR:unknown command NOPE\n", 0)})
        device = open_device(port)
        self.assertEqual(device.command("NOPE", timeout=2), "CMD_ERROR:unknown command NOPE")

    def test_a_command_with_spaces_around_it_gets_its_reply(self):
        port = FakePort(replies={b"CMD: STATE \n": (b"STATE: activity=Home\n", 0)})
        device = open_device(port)
        self.assertEqual(device.command(" STATE ", timeout=2), "STATE: activity=Home")

    def test_a_busy_idle_leaves_the_running_wait_to_answer_first(self):
        port = FakePort(replies={
            b"CMD:IDLE\n": (b"IDLE_ERROR:busy\n", 0),
            b"CMD:IDLE:100\n": (b"IDLE:120\n", 0.3),
        })
        device = open_device(port)
        self.assertEqual(device.command("IDLE", timeout=2), "IDLE_ERROR:busy")
        port.arrive(b"IDLE_TIMEOUT:render\n", after=0.1)  # the wait that was running
        self.assertEqual(device.command("IDLE:100", timeout=2), "IDLE:120")

    def test_after_a_timeout_a_late_reply_of_the_same_kind_is_dropped(self):
        port = FakePort(replies={b"CMD:IDLE:100\n": (b"IDLE:120\n", 0.3)})
        device = open_device(port)
        self.assertIsNone(device.command("IDLE", timeout=0.2))
        port.arrive(b"IDLE:9000\n", after=0.1)  # the late reply, still on its way
        self.assertEqual(device.command("IDLE:100", timeout=2), "IDLE:120")


if __name__ == "__main__":
    unittest.main()
