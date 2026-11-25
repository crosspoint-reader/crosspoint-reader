"""Source contract: Dessen network calls must not enter transport while offline."""
import re
import unittest
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] /
          "src/activities/plugins/CrosspointServices.cpp").read_text(encoding="utf-8")


class DessenNetworkGuardTests(unittest.TestCase):
    def check_guard(self, operation, transport):
        case = SOURCE.split(f"case Op::{operation}: {{", 1)[1].split("case Op::", 1)[0]
        guard = re.search(
            r"if\s*\(WiFi\.status\(\)\s*!=\s*WL_CONNECTED\)\s*\{\s*"
            r"result_\s*=\s*DSN_ERR_IO;\s*break;\s*\}", case
        )
        self.assertIsNotNone(guard)
        self.assertLess(guard.end(), case.index(transport))

    def test_http_requires_connected_wifi(self):
        self.check_guard("Net", "HttpDownloader::request(")

    def test_fetch_requires_connected_wifi(self):
        self.check_guard("Fetch", "HttpDownloader::downloadToFile(")

    def test_error_completes_pending_operation(self):
        service = SOURCE.split("bool CrosspointServices::serviceAsyncWork()", 1)[1]
        self.assertRegex(service, r"\}\s*done_ = true;\s*return true;")
        poll = SOURCE.split("int CrosspointServices::poll()", 1)[1].split("bool CrosspointServices::serviceAsyncWork", 1)[0]
        self.assertRegex(poll, r"pending_ = Op::None;\s*return result_;")


if __name__ == "__main__":
    unittest.main()
