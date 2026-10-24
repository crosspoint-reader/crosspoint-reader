#!/usr/bin/env python3
"""Diff lib/OtShaper against HarfBuzz, glyph for glyph.

Shapes every line of the given text files with HarfBuzz (uharfbuzz) and with
ot_shape_cli (built from test/ot_shaper/OtShapeCli.cpp), and reports every
line whose glyph IDs, advances or offsets differ.

    pip install uharfbuzz
    python3 test/ot_shaper/compare_harfbuzz.py --cli build/ot_shape_cli \\
        --font NotoSansBengali-Regular.ttf words-bn.txt [--lang bn]

Exits non-zero when any line differs.
"""

import argparse
import subprocess
import sys

import uharfbuzz as hb

ISO = ["Deva", "Beng", "Guru", "Gujr", "Orya", "Taml", "Telu", "Knda", "Mlym", "Sinh"]


def script_of(text):
    for ch in text:
        cp = ord(ch)
        if 0x0900 <= cp <= 0x0DFF and not (0x0951 <= cp <= 0x0954 or cp in (0x0964, 0x0965)):
            return ISO[(cp - 0x0900) >> 7]
    return "Deva"


def harfbuzz_lines(font_path, lines, scale, ppem, lang):
    blob = hb.Blob.from_file_path(font_path)
    font = hb.Font(hb.Face(blob))
    font.scale = (scale, scale)
    font.ppem = (ppem, ppem)
    out = []
    for text in lines:
        buf = hb.Buffer()
        buf.add_str(text)
        buf.direction = "ltr"
        buf.script = script_of(text)
        if lang:
            buf.language = lang
        hb.shape(font, buf, {})
        out.append(" ".join(f"{i.codepoint}+{p.x_advance}+{p.x_offset}+{p.y_offset}"
                            for i, p in zip(buf.glyph_infos, buf.glyph_positions)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--font", required=True)
    ap.add_argument("--lang", default="")
    ap.add_argument("--scale", type=int, default=2133)  # 16 pt at 150 DPI in 26.6
    ap.add_argument("--ppem", type=int, default=33)
    ap.add_argument("--show", type=int, default=20, help="mismatches to print")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()

    lines = []
    for path in args.files:
        with open(path, encoding="utf-8") as f:
            lines += [l.strip() for l in f if l.strip() and not l.startswith("#")]
    lines = list(dict.fromkeys(lines))

    expected = harfbuzz_lines(args.font, lines, args.scale, args.ppem, args.lang)
    run = subprocess.run([args.cli, args.font, str(args.scale), str(args.ppem), args.lang or "-"],
                         input="\n".join(lines) + "\n", capture_output=True, text=True, check=True)
    actual = run.stdout.splitlines()

    bad = 0
    for text, exp, act in zip(lines, expected, actual):
        if exp == act:
            continue
        bad += 1
        if bad <= args.show:
            print(f"MISMATCH {text!r} ({' '.join(f'U+{ord(c):04X}' for c in text)})")
            print(f"  harfbuzz: {exp}")
            print(f"  otshaper: {act}")
    print(f"{args.font}: {len(lines) - bad}/{len(lines)} lines identical")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
