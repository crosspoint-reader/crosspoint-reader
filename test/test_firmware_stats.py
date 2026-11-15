import importlib.util
from pathlib import Path
import unittest
import json
import shutil
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
                         dict(environment='default', sha='abc', ram=11, flash=12345))

    def test_saved_tool_works_when_pr_head_lacks_ci_scripts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tool = root / 'saved-parser.py'
            shutil.copy(spec.origin, tool)
            head = root / 'old-pr-head'
            head.mkdir()
            (head / 'pio.log').write_text('RAM: 1% (used 10 bytes from 1000 bytes)')
            self.assertFalse((head / '.github/scripts/firmware-stats.py').exists())
            result = json.loads(subprocess.check_output(
                [sys.executable, str(tool), 'pio.log', 'default', 'a' * 40], cwd=head))
            self.assertEqual(result['ram'], 10)

    def test_missing_and_malformed_are_not_zero(self):
        self.assertIsNone(stats.parse_stats('RAM: used NaN bytes', 'x', 'a')['ram'])
        self.assertIsNone(stats.parse_stats('', 'x', 'a')['flash'])


if __name__ == '__main__':
    unittest.main()
