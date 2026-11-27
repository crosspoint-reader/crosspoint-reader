import importlib.util
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "lib/EpdFont/scripts/dedupe_font_tables.py"
spec = importlib.util.spec_from_file_location("dedupe_font_tables", SCRIPT)
dedupe_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dedupe_module)


def header(name, values="0x0041, 0x0042"):
    return (f'#pragma once\n#include "EpdFontData.h"\n'
            f'static const uint16_t {name}KernLeftCodepoints[] = {{{values}}};\n'
            f'static const uint8_t {name}Bitmaps[2] = {{0x12, 0x34}};\n'
            f'static const EpdFontData {name} = {{{name}KernLeftCodepoints}};\n')


class DedupeFontTablesTest(unittest.TestCase):
    def test_shared_arrays_and_partial_regeneration(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "a.h").write_text(header("a"))
            (root / "b.h").write_text(header("b"))
            (root / "c.h").write_text(header("c", "0x0041, 0x0043"))
            dedupe_module.dedupe(root)
            shared = (root / "SharedFontTables.h").read_text()
            self.assertEqual(shared.count("static const"), 1)
            self.assertIn("0x0041, 0x0042", shared)
            self.assertIn("cKernLeftCodepoints[]", (root / "c.h").read_text())
            self.assertIn("aBitmaps[2] = {0x12, 0x34}", (root / "a.h").read_text())
            snapshot = {p: p.read_text() for p in root.glob("*.h")}
            dedupe_module.dedupe(root)
            self.assertEqual(snapshot, {p: p.read_text() for p in root.glob("*.h")})
            (root / "a.h").write_text(header("a"))
            dedupe_module.dedupe(root)
            self.assertEqual(snapshot, {p: p.read_text() for p in root.glob("*.h")})


if __name__ == "__main__":
    unittest.main()
