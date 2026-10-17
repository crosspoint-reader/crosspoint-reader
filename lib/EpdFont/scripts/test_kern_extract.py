"""Kerning is read the way OpenType applies it, by the Python converters and the browser one.

    python3 lib/EpdFont/scripts/test_kern_extract.py

Needs fontTools (requirements.txt). The browser converter's twin (tools/ftwasm/js) is checked
against the same fonts when node is on the PATH, and skipped otherwise.

The fonts are built here, with feaLib, so each one holds exactly the lookups a case needs.
"""

import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr

sys.dont_write_bytecode = True
SCRIPTS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(SCRIPTS, '..', '..', '..'))
sys.path.insert(0, SCRIPTS)

from fontTools.feaLib.builder import addOpenTypeFeaturesFromString  # noqa: E402
from fontTools.fontBuilder import FontBuilder  # noqa: E402
from fontTools.pens.ttGlyphPen import TTGlyphPen  # noqa: E402
from fontTools.ttLib import TTFont, newTable  # noqa: E402
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0  # noqa: E402

from kern_extract import extract_raw_kerning  # noqa: E402

GLYPHS = ['.notdef', 'A', 'T', 'V', 'a', 'o', 'y']

# feaLib turns lookup PAIRS into: format 1 {V a -80, V o 0}; format 2 over [A T V] (A and V in
# class 0) x [a o] (y in class 0); then, after the break, format 2 over [A y] with A y -50 and
# y a -20. EXT is an Extension lookup holding a format 1 and a format 2 subtable; SINGLE is not
# pair kerning at all.
GPOS_KERN = """
languagesystem DFLT dflt;
lookup PAIRS {
  pos V a -80;
  pos V o 0;
  pos [A V] [a o] -70;
  pos [T] [a o] -30;
  subtable;
  pos [A] [y] -50;
  pos [y] [a] -20;
} PAIRS;
lookup MORE {
  pos V a -10;
} MORE;
lookup EXT useExtension {
  pos T y -40;
  pos [T] [y] -15;
} EXT;
lookup SINGLE {
  pos a -5;
} SINGLE;
feature kern { lookup PAIRS; lookup MORE; lookup EXT; lookup SINGLE; } kern;
"""

LEGACY_KERN = {('A', 'V'): -40, ('V', 'a'): -25}


def build_font(gpos_fea=None, legacy_kern=None):
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(GLYPHS)
    fb.setupCharacterMap({ord(name): name for name in GLYPHS[1:]})
    pen = TTGlyphPen(None)
    pen.moveTo((0, 0))
    pen.lineTo((0, 500))
    pen.lineTo((400, 500))
    pen.closePath()
    glyph = pen.glyph()
    fb.setupGlyf({name: glyph for name in GLYPHS})
    fb.setupHorizontalMetrics({name: (500, 0) for name in GLYPHS})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({'familyName': 'KernTest', 'styleName': 'Regular'})
    fb.setupOS2()
    fb.setupPost()
    if gpos_fea:
        addOpenTypeFeaturesFromString(fb.font, gpos_fea)
    if legacy_kern:
        subtable = KernTable_format_0()
        subtable.version, subtable.coverage, subtable.format, subtable.tupleIndex = 0, 1, 0, None
        subtable.kernTable = dict(legacy_kern)
        kern = newTable('kern')
        kern.version = 0
        kern.kernTables = [subtable]
        fb.font['kern'] = kern
    out = io.BytesIO()
    fb.font.save(out)
    return out.getvalue()


def python_kerning(font_bytes):
    font = TTFont(io.BytesIO(font_bytes))
    with redirect_stderr(io.StringIO()):  # the "skipping lookupType" note for SINGLE
        return extract_raw_kerning(font, set(GLYPHS[1:]))


def browser_kerning(font_bytes, tmp):
    """The same table walk in tools/ftwasm/js, keyed by glyph name."""
    path = os.path.join(tmp, 'font.ttf')
    with open(path, 'wb') as f:
        f.write(font_bytes)
    script = """
import fs from 'node:fs';
import { pathToFileURL } from 'node:url';
const [js, fontPath] = process.argv.slice(1);
const { SfntFont } = await import(pathToFileURL(js + '/otf-tables.mjs').href);
const { prepareKerning } = await import(pathToFileURL(js + '/cpfont-writer.mjs').href);
const font = new SfntFont(new Uint8Array(fs.readFileSync(fontPath)), fontPath);
const { raw } = prepareKerning(font, [...font.bestCmap().keys()]);
console.log(JSON.stringify([...raw].map(([k, v]) => [Math.floor(k / 65536), k % 65536, v])));
"""
    js = os.path.join(REPO, 'tools', 'ftwasm', 'js')
    r = subprocess.run(['node', '--input-type=module', '-e', script, js, path],
                       capture_output=True, text=True, check=True)
    return {(GLYPHS[l], GLYPHS[r]): v for l, r, v in json.loads(r.stdout)}


class FirstMatchTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.kern = python_kerning(build_font(GPOS_KERN))

    def test_pair_exception_wins_over_class_kerning(self):
        # -80 from the format 1 exception plus -10 from the next lookup; summing every subtable
        # also added the class value and gave -160.
        self.assertEqual(self.kern[('V', 'a')], -90)

    def test_zero_valued_exception_still_decides_its_pair(self):
        self.assertNotIn(('V', 'o'), self.kern)  # not the class value, -70

    def test_class_subtable_decides_every_pair_of_its_left_glyphs(self):
        self.assertEqual(self.kern[('A', 'a')], -70)  # A is in class 0 of ClassDef1
        self.assertEqual(self.kern[('T', 'o')], -30)
        # A y: the first class subtable covers A and has 0 for y (class 0 of ClassDef2), so the
        # later -50 never applies.
        self.assertNotIn(('A', 'y'), self.kern)
        # y is not covered by it, so the later subtable kerns y a.
        self.assertEqual(self.kern[('y', 'a')], -20)

    def test_extension_lookup_takes_its_first_match_too(self):
        self.assertEqual(self.kern[('T', 'y')], -40)  # not -55

    def test_no_other_pairs(self):
        self.assertEqual(set(self.kern), {('V', 'a'), ('A', 'a'), ('A', 'o'), ('T', 'a'), ('T', 'o'),
                                          ('y', 'a'), ('T', 'y')})


class LegacyKernTableTest(unittest.TestCase):
    def test_ignored_when_gpos_has_kern(self):
        kern = python_kerning(build_font(GPOS_KERN, LEGACY_KERN))
        self.assertNotIn(('A', 'V'), kern)
        self.assertEqual(kern[('V', 'a')], -90)  # not also the table's -25

    def test_used_without_gpos_kern(self):
        self.assertEqual(python_kerning(build_font(None, LEGACY_KERN)), LEGACY_KERN)


@unittest.skipUnless(shutil.which('node'), 'node is not on the PATH')
class BrowserConverterTest(unittest.TestCase):
    def test_reads_the_same_kerning(self):
        with tempfile.TemporaryDirectory() as tmp:
            for fea, legacy in ((GPOS_KERN, None), (GPOS_KERN, LEGACY_KERN), (None, LEGACY_KERN)):
                font = build_font(fea, legacy)
                with self.subTest(gpos=bool(fea), legacy=bool(legacy)):
                    self.assertEqual(browser_kerning(font, tmp), python_kerning(font))


if __name__ == '__main__':
    unittest.main()
