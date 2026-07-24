#!/usr/bin/env python3

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = REPO_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))

from write_if_changed import write_if_changed


def load_git_branch():
    spec = importlib.util.spec_from_file_location("git_branch", SCRIPTS / "git_branch.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class CodegenTest(unittest.TestCase):
    def test_write_if_changed_preserves_file_when_bytes_match(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "generated.h"
            self.assertTrue(write_if_changed(path, "same\n"))
            before = path.stat()
            self.assertFalse(write_if_changed(path, "same\n"))
            after = path.stat()
            self.assertEqual(before.st_ino, after.st_ino)
            self.assertEqual(before.st_mtime_ns, after.st_mtime_ns)
            self.assertTrue(write_if_changed(path, "changed\n"))
            self.assertEqual(path.read_text(encoding="utf-8"), "changed\n")

    def test_html_codegen_is_reproducible_and_does_not_rewrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "src"
            source.mkdir()
            (source / "sample.html").write_text("<p>CrossVi</p>\n", encoding="utf-8")
            command = [sys.executable, str(SCRIPTS / "build_html.py")]
            subprocess.run(command, cwd=root, check=True, capture_output=True, text=True)
            generated = source / "sampleHtml.generated.h"
            before = generated.stat()
            payload = generated.read_bytes()
            subprocess.run(command, cwd=root, check=True, capture_output=True, text=True)
            after = generated.stat()
            self.assertEqual(payload, generated.read_bytes())
            self.assertEqual(before.st_ino, after.st_ino)
            self.assertEqual(before.st_mtime_ns, after.st_mtime_ns)

    def test_version_mapping_preserves_existing_environment_contract(self):
        module = load_git_branch()
        self.assertEqual(module.compute_version("gh_release", str(REPO_ROOT)), "1.4.1")
        self.assertEqual(module.compute_version("slim", str(REPO_ROOT)), "1.4.1-slim")
        self.assertEqual(module.compute_version("simulator_x3", str(REPO_ROOT)), "1.4.1-simulator")
        self.assertEqual(module.compute_version("simulator_x4", str(REPO_ROOT)), "1.4.1-simulator")


if __name__ == "__main__":
    unittest.main()
