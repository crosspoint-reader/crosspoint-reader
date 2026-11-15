import importlib.util
from pathlib import Path
import unittest
import json
import subprocess
import sys
import tempfile

spec = importlib.util.spec_from_file_location('stats', Path(__file__).resolve().parents[1] / '.github/scripts/firmware-stats.py')
stats = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stats)


class StatsTest(unittest.TestCase):
    def test_final_measurement_and_exact_bytes(self):
        log = 'RAM: [==] 2% (used 10 bytes from 200 bytes)\nRAM: [==] 3% (used 11 bytes from 200 bytes)\nFlash: [==] 4% (used 12345 bytes from 99999 bytes)'
        self.assertEqual(stats.parse_stats(log, 'default', 'abc'),
                         dict(environment='default', sha='abc', ram=11, flash=12345, image=None, base_sha=None))

    def test_final_image_includes_appended_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / 'firmware.bin'
            log = root / 'pio.log'
            log.write_text('Flash: used 100 bytes from 1000 bytes')
            image.write_bytes(b'x' * 100)
            def measure():
                return json.loads(subprocess.check_output([
                    sys.executable, spec.origin, str(log), 'default', 'a' * 40,
                    str(image), 'b' * 40]))
            before = measure()
            with image.open('ab') as output:
                output.write(b'symbols' * 100)
            after = measure()
            self.assertEqual(after['image'] - before['image'], 700)
            self.assertEqual(after['flash'], before['flash'])
            self.assertEqual(after['base_sha'], 'b' * 40)

    def test_missing_and_malformed_are_not_zero(self):
        self.assertIsNone(stats.parse_stats('RAM: used NaN bytes', 'x', 'a')['ram'])
        self.assertIsNone(stats.parse_stats('', 'x', 'a')['flash'])


if __name__ == '__main__':
    unittest.main()
