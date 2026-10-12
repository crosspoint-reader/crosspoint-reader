"""Make test fonts with a legacy 'kern' table (no font in the repo has one).

    python -I -B make_kern_fonts.py <source.ttf> <out-dir>

Writes three fonts derived from <source.ttf>:
  KernTest-Regular.ttf     GPOS kept + OpenType 'kern' v0, one format-0 subtable
                           (pairs overlap GPOS pairs, so the script adds both)
  KernTest-Italic.ttf      GPOS removed + 'kern' v0 with two format-0 subtables
                           (length field honoured) and one format-2 stub (ignored)
  KernTest-BoldItalic.ttf  GPOS removed + Apple 'kern' version 1.0, one subtable
"""
import os
import random
import sys

from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0, KernTable_format_unkown


def pairs_for(font, seed, n):
    cmap = font.getBestCmap()
    letters = [cmap[c] for c in list(range(0x41, 0x5B)) + list(range(0x61, 0x7B)) + [0x2C, 0x2E, 0x22, 0x27]
               if c in cmap]
    rnd = random.Random(seed)
    table = {}
    while len(table) < n:
        a, b = rnd.choice(letters), rnd.choice(letters)
        # values that hit exact halves at some sizes plus ordinary ones
        table[(a, b)] = rnd.choice([-64, -192, 64, -100, -37, -12, 25, -250, -5])
    table[("A", "V")] = -80  # overlaps GPOS kerning in most Latin fonts
    table[("T", "o")] = -70
    return table


def subtable(pairs, apple=False, coverage=1):
    st = KernTable_format_0(apple=apple)
    st.coverage = coverage
    st.format = 0
    st.tupleIndex = 0 if apple else None
    st.kernTable = dict(pairs)
    return st


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)

    f = TTFont(src)
    kern = newTable("kern")
    kern.version = 0
    kern.kernTables = [subtable(pairs_for(f, 1, 300))]
    f["kern"] = kern
    f.save(os.path.join(out, "KernTest-Regular.ttf"))

    f = TTFont(src)
    del f["GPOS"]
    kern = newTable("kern")
    kern.version = 0
    stub = KernTable_format_unkown(2)
    stub.data = b"\x00\x00\x00\x0e\x02\x01" + b"\x00" * 8  # version, length=14, format 2, coverage
    kern.kernTables = [subtable(pairs_for(f, 2, 200)), stub, subtable(pairs_for(f, 3, 150))]
    f["kern"] = kern
    f.save(os.path.join(out, "KernTest-Italic.ttf"))

    f = TTFont(src)
    del f["GPOS"]
    kern = newTable("kern")
    kern.version = 1.0
    kern.kernTables = [subtable(pairs_for(f, 4, 250), apple=True, coverage=0)]
    f["kern"] = kern
    f.save(os.path.join(out, "KernTest-BoldItalic.ttf"))
    for name in ("Regular", "Italic", "BoldItalic"):
        t = TTFont(os.path.join(out, f"KernTest-{name}.ttf"))
        print(name, "kern version", t["kern"].version, "subtables",
              [getattr(s, "format", "?") for s in t["kern"].kernTables], "GPOS" in t)


if __name__ == "__main__":
    main()
