"""CLI acceptance checks against the real HTTP server and a scripted serial device."""

import json
import subprocess
import sys
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

import test_debugging_monitor as monitor_tests
from device_control import ControlError, ControlTimeout, DeviceControl
from monitor_server import create_server

SCRIPTS = Path(__file__).resolve().parents[1]


class WaitDeadlineTests(unittest.TestCase):
    def test_confirmed_wrong_state_at_deadline_is_a_mismatch(self):
        device = DeviceControl()
        now = [0.0]

        def reply():
            now[0] = 1.0
            return {"available": 1, "busy": 0, "activity": "Home"}

        with (
            patch("device_control.time.monotonic", side_effect=lambda: now[0]),
            patch("device_control.time.sleep"),
            patch.object(device, "state", side_effect=reply),
            self.assertRaises(ControlError) as failure,
        ):
            device.wait_ready(lambda state: state["activity"] == "Settings", 1)
        self.assertEqual(failure.exception.code, 7)

    def test_unanswered_poll_stays_a_timeout_after_a_ready_mismatch(self):
        device = DeviceControl()
        with (
            patch("device_control.time.sleep"),
            patch.object(
                device,
                "state",
                side_effect=[
                    {"available": 1, "busy": 0, "activity": "Home"},
                    ControlTimeout("No STATE reply"),
                ],
            ),
            self.assertRaises(ControlError) as failure,
        ):
            device.wait_ready(lambda state: state["activity"] == "Settings", 1)
        self.assertEqual(failure.exception.code, 5)
        self.assertEqual(str(failure.exception), "No STATE reply")


class CliTests(monitor_tests.SessionFixture):
    def setUp(self):
        super().setUp()
        self.last_id = 0
        self.page = 1
        self.mode = "normal"
        self.ports[-1].on_write = self.firmware
        self.server = create_server(self.session, 0)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop_server)

    def stop_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def firmware(self, data):
        fields = data.decode().strip().removeprefix("CMD:").split()
        verb = fields[0]
        if verb == "SCREENSHOT":
            self.ports[-1].incoming.put(
                b"SCREENSHOT_META:1:8:2:1:90:0\n"
                b"SCREENSHOT_START:2\n\x00\xffSCREENSHOT_END\n"
            )
            return
        request_id = int(fields[1])
        if verb == "INFO":
            self.reply(request_id, f"INFO protocol=1 last_id={self.last_id}")
            return
        if request_id <= self.last_id:
            self.reply(request_id, "ERROR reason=STALE_ID")
            return
        self.last_id = request_id
        if verb == "STATE":
            self.reply(
                request_id,
                f"STATE available=1 busy=0 activity=EpubReader reader=1 "
                f"spine=0 page={self.page} pages=100 requested=2 completed=2",
            )
        elif verb == "PRESS":
            self.reply(request_id, "ACCEPTED")
            if self.mode == "silent":
                return
            if self.mode != "ignored":
                self.page += 1
            self.reply(request_id, "INPUT_DONE held_ms=80")
            if self.mode == "crash":
                self.ports[-1].incoming.put(b"abort() was called at PC 0x1234\n")
        elif verb == "CANCEL":
            self.reply(request_id, "CANCELLED")

    def reply(self, request_id, event):
        self.ports[-1].incoming.put(
            f"[123] [INF] [CTL] id={request_id} boot=12 event={event}\n".encode()
        )

    def cli(self, *args, stdin=None, expected=0):
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPTS / "device_control.py"),
                "--url",
                f"http://127.0.0.1:{self.server.server_port}",
                "--json",
                "--timeout",
                "2",
                *args,
            ],
            input=stdin,
            text=True,
            capture_output=True,
            check=False,
            timeout=8,
        )
        expected_codes = (expected,) if isinstance(expected, int) else expected
        self.assertIn(result.returncode, expected_codes, result.stdout + result.stderr)
        return [json.loads(line) for line in result.stdout.splitlines()]

    def test_batch_one_lease_repeated_verified_input_and_raw_id_resync(self):
        with patch.object(self.session, "claim", wraps=self.session.claim) as claim:
            result = self.cli(
                "run",
                "-",
                stdin="wait\npress page-forward --repeat 2 --expect-page-change\n"
                "send 'STATE 100'\nstate\ncancel\n",
            )
        self.assertEqual(claim.call_count, 1)
        self.assertEqual([r["state"]["page"] for r in result if "input" in r], [2, 3])
        self.assertFalse(result[3]["execution_confirmed"])
        self.assertEqual(result[4]["id"], 101)
        self.assertEqual(result[-1]["event"], "CANCELLED")
        self.assertFalse(self.session.status()["control_claimed"])

    def test_timeout_does_not_replay_input_or_execute_next_step(self):
        self.mode = "silent"
        result = self.cli(
            "run", "-", stdin="press confirm --timeout 0.4\ncancel\n", expected=5
        )
        self.assertEqual(result[-1]["step"], 1)
        self.assertEqual(result[-1]["last_command"]["stage"], "accepted")
        self.assertEqual(result[-1]["output_dir"], str(Path(self.temp.name).resolve()))
        self.assertEqual(sum(b"PRESS" in p for p in self.ports[-1].writes), 1)
        self.assertFalse(any(b"CANCEL" in p for p in self.ports[-1].writes))
        self.assertFalse(self.session.status()["control_claimed"])

    def test_ignored_input_fails_expectation_but_delivery_only_succeeds(self):
        self.mode = "ignored"
        result = self.cli(
            "press",
            "page-forward",
            "--expect-page-change",
            "--timeout",
            "0.4",
            # Wall-clock expiry may interrupt the final HTTP query. Exact deadline
            # classification is covered deterministically by WaitDeadlineTests.
            expected=(5, 7),
        )
        error = result[-1]
        self.assertFalse(error["ok"])
        self.assertEqual(error["last_state"]["page"], 1)
        self.assertEqual(error["last_state"]["busy"], 0)
        self.assertTrue(error["last_command"]["command"].startswith("STATE "))
        self.assertEqual(sum(b"PRESS" in p for p in self.ports[-1].writes), 1)
        self.assertFalse(self.session.status()["control_claimed"])
        result = self.cli("press", "page-forward", "--wait", "input")
        self.assertNotIn("state", result[0])
        self.assertEqual(result[0]["input"]["event"], "INPUT_DONE")

    def test_crash_aborts_batch_even_after_input_done(self):
        self.mode = "crash"
        result = self.cli("run", "-", stdin="press confirm\ncancel\n", expected=6)
        self.assertEqual(result[-1]["boot"], 12)
        self.assertFalse(any(b"CANCEL" in p for p in self.ports[-1].writes))
        self.assertFalse(self.session.status()["control_claimed"])

    def test_parse_entire_batch_before_mutation_and_reject_multiline_send(self):
        self.cli("run", "-", stdin="press confirm\npress power\n", expected=2)
        self.cli("send", "STATE 1\n", expected=2)
        self.assertEqual(self.ports[-1].writes, [])
        self.assertFalse(self.session.status()["control_claimed"])

    def test_invalid_late_batch_arguments_do_not_send_earlier_input(self):
        for step in (
            "send ''",
            "send '   '",
            "send 'CMD:'",
            "send 'STATE \t1'",
            "send '한글'",
            "send '" + "x" * 124 + "'",
            "screenshot capture.jpg",
        ):
            with self.subTest(step=step):
                self.ports[-1].writes.clear()
                result = self.cli(
                    "run", "-", stdin=f"press confirm\n{step}\n", expected=2
                )
                self.assertEqual(self.ports[-1].writes, [])
                self.assertIn("Line 2:", result[-1]["error"])
                self.assertFalse(self.session.status()["control_claimed"])

    def test_screenshot_download_is_rotated_and_failure_keeps_destination(self):
        path = Path(self.temp.name) / "capture.pbm"
        result = self.cli("screenshot", str(path))
        self.assertEqual((result[0]["width"], result[0]["height"]), (2, 8))
        self.assertEqual(path.read_bytes(), b"P4\n2 8\n" + b"\x40" * 8)
        self.ports[-1].on_write = lambda _data: None
        self.cli("screenshot", str(path), "--timeout", "0.3", expected=5)
        self.assertEqual(path.read_bytes(), b"P4\n2 8\n" + b"\x40" * 8)
        self.assertEqual(list(path.parent.glob(".device-*")), [])

    def test_read_only_access_during_lease_and_serial_handoff_without_firmware(self):
        lease = self.session.claim()
        self.cli("status")
        self.cli("logs")
        self.cli("info", expected=4)
        self.assertEqual(self.ports[-1].writes, [])
        self.session.unclaim(lease["token"])
        self.cli("serial", "release")
        self.assertTrue(self.ports[-1].closed)
        self.cli("serial", "reconnect")
        self.assertEqual(len(self.ports), 2)
        self.assertEqual(self.ports[-1].writes, [])


if __name__ == "__main__":
    unittest.main()
