"""Regressions for serial framing, capture orientation, and port ownership."""

import http.client
import json
import os
import queue
import select
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from concurrent.futures import Future
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

from monitor_screenshot import framebuffer_to_pbm
from monitor_server import create_server
from monitor_session import DeviceSession, SessionError


class FakeSerial:
    def __init__(self):
        self.incoming = queue.Queue()
        self.writes = []
        self.closed = False
        self.failure = False
        self.on_write = None

    @property
    def in_waiting(self):
        return 1

    def read(self, _size):
        if self.failure:
            raise OSError("device removed")
        try:
            return self.incoming.get(timeout=0.01)
        except queue.Empty:
            return b""

    def write(self, data):
        if self.failure:
            raise OSError("write failed")
        self.writes.append(data)
        if self.on_write:
            self.on_write(data)
        return len(data)

    def close(self):
        self.closed = True


class SessionFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.ports = []
        self.available = True

        def factory(_port, _baud):
            if not self.available:
                raise OSError("device unavailable")
            # Every reconnection must close the previous handle first.
            self.assertTrue(all(p.closed for p in self.ports))
            port = FakeSerial()
            self.ports.append(port)
            return port

        self.session = DeviceSession(
            "fake", 115200, self.temp.name, serial_factory=factory
        )
        self.session.start()
        self.addCleanup(self.session.close)
        self.wait_for(lambda: self.session.status()["state"] == "connected")

    def wait_for(self, condition, timeout=3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if condition():
                return
            time.sleep(0.005)
        self.fail("Condition did not become true")

    def events(self, kind):
        return [e for e in self.session.read_events()["events"] if e["type"] == kind]


class SessionTests(SessionFixture):
    def test_control_replies_preserve_logs_and_report_rejected_capture(self):
        self.ports[-1].incoming.put(
            b"[123] [INF] [CTL] id=4 boot=12 event=STATE available=1 busy=0 "
            b"activity=Home spine=-1 page=0\n"
        )
        self.wait_for(lambda: self.events("control"))
        reply = self.events("control")[0]
        self.assertEqual(
            (reply["id"], reply["boot"], reply["event"], reply["spine"]),
            (4, 12, "STATE", -1),
        )
        self.assertIn("[123] [INF] [CTL]", self.events("log")[0]["line"])
        sent = self.session.request("command", "SCREENSHOT")
        self.ports[-1].incoming.put(
            b"[124] [INF] [CTL] id=0 boot=12 event=ERROR reason=BUSY\n"
        )
        self.wait_for(lambda: self.events("screenshot_error"))
        self.assertEqual(
            self.events("screenshot_error")[0]["command_id"], sent["command_id"]
        )
        self.assertFalse(self.session.waiting_screenshot)

    def test_metadata_orientation_and_missing_metadata_rejected(self):
        self.session.request("command", "SCREENSHOT")
        self.ports[-1].incoming.put(
            b"SCREENSHOT_META:1:5:3:2:90:1\nSCREENSHOT_START:6\n"
            b"\x7f\x00\x9f\x00\xef\x00SCREENSHOT_END\n"
        )
        self.wait_for(lambda: len(self.events("screenshot")) == 1)
        capture = self.events("screenshot")[0]
        self.assertEqual((capture["width"], capture["height"]), (3, 5))
        self.assertEqual(
            (capture["rotation"], capture["inverted"]),
            (90, True),
        )
        self.assertEqual(
            Path(capture["raw_path"]).read_bytes(), b"\x7f\x00\x9f\x00\xef\x00"
        )
        self.session.request("command", "SCREENSHOT")
        self.ports[-1].incoming.put(b"SCREENSHOT_START:2\n\x00\xffSCREENSHOT_END\n")
        self.wait_for(lambda: self.events("screenshot_error"))
        self.assertEqual(len(self.events("screenshot")), 1)
        self.assertIn("requires SCREENSHOT_META", self.session.status()["error"])

    def test_mismatched_metadata_rejects_capture(self):
        self.session.request("command", "SCREENSHOT")
        self.ports[-1].incoming.put(
            b"SCREENSHOT_META:1:8:2:1:90:0\nSCREENSHOT_START:3\n"
        )
        self.wait_for(lambda: self.events("screenshot_error"))
        self.assertFalse(self.events("screenshot"))

    def test_disconnect_drops_partial_line_and_reconnects(self):
        self.ports[-1].incoming.put(b"unfinished")
        self.wait_for(lambda: bool(self.session.pending))
        self.available = False
        self.ports[-1].failure = True
        self.wait_for(lambda: self.session.status()["state"] == "disconnected")
        with self.assertRaises(SessionError):
            self.session.request("command", "STATE")
        self.available = True
        self.wait_for(lambda: len(self.ports) == 2)
        self.ports[-1].incoming.put(b"new boot\n")
        self.wait_for(lambda: bool(self.events("log")))
        self.assertEqual(self.events("log")[-1]["line"], "new boot")

    def test_screenshot_binary_newlines_trailer_and_following_log(self):
        response = self.session.request("command", "SCREENSHOT")
        with self.assertRaises(SessionError):
            self.session.request("command", "SCREENSHOT")
        chunks = [
            b"SCREENSHOT_META:1:8:2:1:0:0\nSCREENSHOT_STA",
            b"RT:2\n",
            b"\n\xffSCREENSHOT_EN",
            b"D\n[123] [DBG] [ERS] Page re",
            b"nder: prewarm=12ms\r\n",
        ]
        for chunk in chunks:
            self.ports[-1].incoming.put(chunk)
        self.wait_for(lambda: bool(self.events("screenshot")))
        capture = self.events("screenshot")[0]
        self.assertEqual(capture["command_id"], response["command_id"])
        self.assertEqual(Path(capture["raw_path"]).read_bytes(), b"\n\xff")
        self.assertEqual(Path(capture["image_path"]).read_bytes(), b"P4\n8 2\n\xf5\x00")
        self.wait_for(lambda: bool(self.events("log")))
        self.assertEqual(
            self.events("log")[-1]["line"],
            "[123] [DBG] [ERS] Page render: prewarm=12ms",
        )
        self.assertEqual(
            (Path(self.temp.name) / "serial.bin").read_bytes(), b"".join(chunks)
        )
        self.session.request("command", "STATE")

    def test_bad_screenshot_trailer_is_not_published_as_image(self):
        self.session.request("command", "SCREENSHOT")
        self.ports[-1].incoming.put(
            b"SCREENSHOT_META:1:8:2:1:0:0\nSCREENSHOT_START:2\nABbad trailer\n"
        )
        self.wait_for(lambda: bool(self.events("screenshot_error")))
        self.assertEqual(self.events("screenshot"), [])
        self.assertFalse(list(Path(self.temp.name).glob("*.pbm")))

    def test_screenshot_timeout_and_invalid_size(self):
        self.session.SCREENSHOT_TIMEOUT = 0.05
        self.session.request("command", "SCREENSHOT")
        self.wait_for(lambda: bool(self.events("screenshot_error")))
        self.assertEqual(self.session.status()["state"], "disconnected")
        self.wait_for(lambda: len(self.ports) == 2)
        self.ports[-1].incoming.put(b"SCREENSHOT_START:999999999\n")
        self.wait_for(lambda: self.session.status()["state"] == "disconnected")
        self.assertEqual(self.events("screenshot"), [])

    def test_commands_are_serialized_and_immediate_response_is_not_lost(self):
        self.ports[-1].on_write = lambda _data: self.ports[-1].incoming.put(b"ack\n")
        from concurrent.futures import ThreadPoolExecutor

        with ThreadPoolExecutor(max_workers=8) as pool:
            results = list(
                pool.map(
                    lambda i: self.session.request("command", f"TEST {i}"), range(8)
                )
            )
        self.assertEqual(len({r["command_id"] for r in results}), 8)
        self.assertEqual(
            set(self.ports[-1].writes), {f"CMD:TEST {i}\n".encode() for i in range(8)}
        )
        self.wait_for(lambda: len(self.events("log")) == 8)
        for result in results:
            self.assertTrue(
                any(
                    e["type"] == "log"
                    for e in self.session.read_events(result["after"])["events"]
                )
            )

    def test_control_is_rechecked_for_queued_commands(self):
        future = Future()
        with self.session.condition:
            self.session.jobs.put((future, "command", "STATE", None))
            self.session.claim()
        with self.assertRaises(SessionError):
            future.result(timeout=1)
        self.assertFalse(self.ports[-1].writes)


class HttpTests(SessionFixture):
    def setUp(self):
        super().setUp()
        self.server = create_server(self.session, 0)
        self.server_thread = threading.Thread(
            target=self.server.serve_forever, daemon=True
        )
        self.server_thread.start()
        self.addCleanup(self.stop_server)

    def stop_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.server_thread.join()

    def http(self, method, path, body=None, token=None, headers=None):
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.server.server_port, timeout=3
        )
        request_headers = {"Content-Type": "application/json"}
        if token:
            request_headers["X-Control-Token"] = token
        request_headers.update(headers or {})
        connection.request(
            method,
            path,
            json.dumps(body) if body is not None else None,
            request_headers,
        )
        response = connection.getresponse()
        data = response.read()
        status = response.status
        connection.close()
        return status, json.loads(data)

    def test_api_control_events_and_flash_handoff(self):
        status, snapshot = self.http("GET", "/status")
        self.assertEqual(status, 200)
        self.assertEqual(snapshot["state"], "connected")
        status, lease = self.http("POST", "/control/claim", {})
        self.assertEqual(status, 200)
        self.assertEqual(self.http("POST", "/command", {"command": "STATE"})[0], 409)
        token = lease["token"]
        status, sent = self.http("POST", "/command", {"command": "STATE"}, token)
        self.assertEqual(status, 200)
        self.ports[-1].incoming.put(b"[STATE] Home\n")
        self.wait_for(lambda: bool(self.events("log")))
        _, events = self.http("GET", f"/events?after={sent['after']}&wait=1")
        self.assertTrue(any(e.get("line") == "[STATE] Home" for e in events["events"]))
        self.assertEqual(
            self.http("POST", "/serial/release", {}, token)[1]["state"], "released"
        )
        self.assertTrue(self.ports[-1].closed)
        self.assertEqual(self.http("POST", "/serial/reconnect", {}, token)[0], 200)
        self.wait_for(lambda: len(self.ports) == 2)
        self.assertEqual(self.http("POST", "/control/release", {}, token)[0], 200)


class ScreenshotTests(unittest.TestCase):
    def test_all_rotations_padded_rows_and_inversion(self):
        # A deliberately asymmetric 5x3 image with an extra padding byte per row.
        data = b"\x7f\x00\x9f\x00\xef\x00"
        expected = {
            0: ["#....", ".##..", "...#."],
            90: ["..#", ".#.", ".#.", "#..", "..."],
            180: [".#...", "..##.", "....#"],
            270: ["...", "..#", ".#.", ".#.", "#.."],
        }
        for angle, rows in expected.items():
            for inverted in (False, True):
                with self.subTest(angle=angle, inverted=inverted):
                    pbm, width, height = framebuffer_to_pbm(
                        data, 5, 3, 2, angle, inverted
                    )
                    pixels = pbm.split(b"\n", 2)[2]
                    stride = (width + 7) // 8
                    actual = [
                        "".join(
                            "#"
                            if pixels[y * stride + x // 8] & (0x80 >> (x % 8))
                            else "."
                            for x in range(width)
                        )
                        for y in range(height)
                    ]
                    wanted = (
                        [row.translate(str.maketrans("#.", ".#")) for row in rows]
                        if inverted
                        else rows
                    )
                    self.assertEqual(actual, wanted)


class MonitorTests(unittest.TestCase):
    @unittest.skipUnless(
        os.name == "posix", "Pseudo-terminal smoke test requires POSIX"
    )
    def test_headless_process_with_real_serial_transport(self):
        self._check_process_with_real_serial_transport(headless=True)

    @unittest.skipUnless(
        os.name == "posix", "Pseudo-terminal smoke test requires POSIX"
    )
    def test_graph_process_with_real_serial_transport(self):
        import importlib.util

        if importlib.util.find_spec("matplotlib") is None:
            self.skipTest("Optional end-to-end graph test requires matplotlib")
        self._check_process_with_real_serial_transport(headless=False)

    def _check_process_with_real_serial_transport(self, headless):
        try:
            import serial
        except ImportError:
            self.skipTest("Optional end-to-end test requires pyserial")
        import pty

        with tempfile.TemporaryDirectory() as directory:
            master, slave = pty.openpty()
            self.addCleanup(os.close, master)
            self.addCleanup(os.close, slave)
            with open(Path(directory) / "stdout.log", "w+") as output:
                process = subprocess.Popen(
                    [
                        sys.executable,
                        "-u",
                        str(SCRIPTS / "debugging_monitor.py"),
                        os.ttyname(slave),
                        "--serve",
                        *(["--headless"] if headless else []),
                        "--http-port",
                        "0",
                        "--output-dir",
                        directory,
                    ],
                    stdout=output,
                    stderr=subprocess.STDOUT,
                    env={**os.environ, "MPLBACKEND": "Agg"},
                )
                try:
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        output.seek(0)
                        logs = output.read()
                        if "Device API:" in logs:
                            break
                        if process.poll() is not None:
                            self.fail(logs)
                        time.sleep(0.02)
                    else:
                        self.fail("Server did not start: " + logs)
                    port = int(
                        logs.split("Device API: http://127.0.0.1:")[1].splitlines()[0]
                    )

                    def api(method, path, body=None):
                        client = http.client.HTTPConnection(
                            "127.0.0.1", port, timeout=3
                        )
                        try:
                            client.request(
                                method,
                                path,
                                json.dumps(body) if body is not None else None,
                                {"Content-Type": "application/json"},
                            )
                            reply = client.getresponse()
                            data = json.loads(reply.read())
                            self.assertEqual(reply.status, 200, data)
                            return data
                        finally:
                            client.close()

                    while api("GET", "/status")["state"] != "connected":
                        if time.monotonic() >= deadline:
                            self.fail("PTY serial connection did not open")
                        time.sleep(0.02)
                    sent = api("POST", "/screenshot", {})
                    self.assertTrue(select.select([master], [], [], 2)[0])
                    self.assertEqual(os.read(master, 256), b"CMD:SCREENSHOT\n")
                    os.write(
                        master,
                        b"SCREENSHOT_META:1:8:2:1:0:0\n"
                        b"SCREENSHOT_START:2\n\x00\xffSCREENSHOT_END\n[100] [MEM] Free: 500\n",
                    )
                    cursor = sent["after"]
                    captures = []
                    while time.monotonic() < deadline and not captures:
                        batch = api("GET", f"/events?after={cursor}&wait=1")
                        cursor = batch["cursor"]
                        captures = [
                            e for e in batch["events"] if e["type"] == "screenshot"
                        ]
                    self.assertTrue(captures)
                    self.assertEqual(
                        Path(captures[0]["raw_path"]).read_bytes(), b"\x00\xff"
                    )
                    self.assertEqual(
                        api("POST", "/serial/release", {})["state"], "released"
                    )
                    # A second serial owner can open after release completes.
                    with serial.Serial(os.ttyname(slave), exclusive=True, timeout=0.1):
                        pass
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                        self.fail("Monitor failed to shut down")
                output.seek(0)
                self.assertEqual(process.returncode, 0, output.read())


if __name__ == "__main__":
    unittest.main()
