"""Check shared built-in tables and the full-width fallback on the real font headers."""

from pathlib import Path
import re
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "lib/EpdFont/scripts"))
import share_builtin_tables as share


class BuiltinFontTablesTest(unittest.TestCase):
    def test_aliases_preserve_types_and_values(self):
        source = ROOT / "lib/EpdFont/builtinFonts"
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp)
            count = share.generate(source, out)
            self.assertGreater(count, 0)
            tables = dict((name, (kind, body)) for kind, name, body in re.findall(
                r"static const (\w+) (table_\w+)\[\] = \{\n(.*?)^\};",
                (out / "BuiltinFontTables.generated.h").read_text(), re.M | re.S))
            self.assertEqual(len(tables), count)
            for name in share.FACE_INCLUDE.findall((source / "all.h").read_text()):
                before, after = (source / name).read_text(), (out / name).read_text()
                for table in share.TABLE.finditer(before):
                    if table[0] in after:
                        continue
                    alias = re.search(r"auto& " + table[2] + r" = builtin_font_tables::(\w+);", after)
                    self.assertIsNotNone(alias, table[2])
                    kind, body = tables[alias[1]]
                    self.assertEqual(kind, table[1])
                    self.assertEqual(share.COMMENT.sub("", body).split(), share.COMMENT.sub("", table[3]).split())

    def test_face_that_exceeds_packed_fields_keeps_original_records(self):
        font = """static const EpdGlyph wideGlyphs[] = {
    { 8, 8, 128, -1, 8, 8, 0 },
};
static const EpdFontData wide = {
    bitmap, wideGlyphs, intervals, 1, 8, 8, 0, false,
};
"""
        self.assertIn("EpdBuiltinGlyph", share.compact_glyphs("wide", font))
        for row in ("-129, 8, 8, 0", "-1, 128, 8, 0", "-1, 8, 8, 65536", "-1, 8, 9, 0"):
            wide = font.replace("-1, 8, 8, 0", row)
            self.assertEqual(share.compact_glyphs("wide", wide), wide)


if __name__ == "__main__":
    unittest.main()
