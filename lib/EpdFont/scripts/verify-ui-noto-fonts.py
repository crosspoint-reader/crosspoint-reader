#!/usr/bin/env python3
"""Verify optical Noto UI faces against the readable 1-bit reference weight."""

import re
from pathlib import Path

import freetype
from fontTools.ttLib import TTFont


SOURCE_DIR = Path(__file__).resolve().parent.parent / "builtinFonts" / "source"
SIZES = (10, 12)
MIN_INK_RATIO = 0.90
MAX_INK_RATIO = 1.10
MAX_MEAN_ADVANCE_DELTA = 0.03
MAX_GLYPH_ADVANCE_DELTA = 0.16

HEBREW_CODEPOINTS = tuple(range(0x05D0, 0x05EB))
ARABIC_INTERVALS = (
    (0x060C, 0x060C),
    (0x061B, 0x061B),
    (0x061F, 0x061F),
    (0x0621, 0x0621),
    (0x0640, 0x0640),
    (0x0660, 0x0669),
    (0x06BA, 0x06BA),
    (0x06D4, 0x06D4),
    (0x06F0, 0x06F9),
    (0xFB56, 0xFB59),
    (0xFB66, 0xFB69),
    (0xFB7A, 0xFB7D),
    (0xFB88, 0xFB95),
    (0xFB9E, 0xFB9F),
    (0xFBA6, 0xFBB1),
    (0xFBFC, 0xFBFF),
    (0xFE80, 0xFEFC),
)
ARABIC_CODEPOINTS = tuple(
    codepoint
    for first, last in ARABIC_INTERVALS
    for codepoint in range(first, last + 1)
)

# The trailing flag mirrors --autohint-font in convert-builtin-fonts.sh: a face
# is checked against the same rasterisation the converter gives it.
FACES = (
    ("Hebrew medium", "NotoSansHebrew", "Regular", "UIMedium", HEBREW_CODEPOINTS, True),
    ("Hebrew bold", "NotoSansHebrew", "Bold", "UIBold", HEBREW_CODEPOINTS, True),
    ("Arabic medium", "NotoSansArabic", "Regular", "UIMedium", ARABIC_CODEPOINTS, False),
    ("Arabic bold", "NotoSansArabic", "Bold", "UIBold", ARABIC_CODEPOINTS, False),
)


def ink_pixels(
    path: Path,
    codepoints: tuple[int, ...],
    size: int,
    monochrome: bool,
    autohint: bool = False,
) -> tuple[int, set[int]]:
    face = freetype.Face(str(path))
    face.set_char_size(size << 6, size << 6, 150, 150)
    ink = 0
    covered = set()

    for codepoint in codepoints:
        if not face.get_char_index(codepoint):
            continue
        covered.add(codepoint)
        flags = freetype.FT_LOAD_RENDER
        if monochrome:
            flags |= freetype.FT_LOAD_TARGET_MONO
        if autohint:
            flags |= freetype.FT_LOAD_FORCE_AUTOHINT
        face.load_char(chr(codepoint), flags)
        bitmap = face.glyph.bitmap
        pitch = abs(bitmap.pitch)
        if monochrome:
            ink += sum(
                (bitmap.buffer[y * pitch + (x >> 3)] >> (7 - (x & 7))) & 1
                for y in range(bitmap.rows)
                for x in range(bitmap.width)
            )
        else:
            ink += sum(
                bitmap.buffer[y * pitch + x] >= 32
                for y in range(bitmap.rows)
                for x in range(bitmap.width)
            )

    return ink, covered


def advance_deltas(reference_path: Path, optical_path: Path, codepoints: tuple[int, ...]) -> tuple[float, float]:
    reference = TTFont(reference_path)
    optical = TTFont(optical_path)
    try:
        if "fvar" in optical or "gvar" in optical:
            raise RuntimeError(f"{optical_path} is not a static font instance")
        reference_cmap = reference.getBestCmap()
        optical_cmap = optical.getBestCmap()
        deltas = []
        for codepoint in codepoints:
            if codepoint not in reference_cmap:
                continue
            if codepoint not in optical_cmap:
                raise RuntimeError(f"{optical_path} is missing U+{codepoint:04X}")
            reference_advance = reference["hmtx"][reference_cmap[codepoint]][0]
            optical_advance = optical["hmtx"][optical_cmap[codepoint]][0]
            if reference_advance:
                deltas.append((optical_advance - reference_advance) / reference_advance)
        return sum(deltas) / len(deltas), max(abs(delta) for delta in deltas)
    finally:
        optical.close()
        reference.close()


def generated_header_ink(
    path: Path,
    stem: str,
    expected_source: str,
    codepoints: tuple[int, ...],
) -> tuple[int, set[int]]:
    header = path.read_text()
    if expected_source not in header.splitlines()[5]:
        raise RuntimeError(f"{path} was not generated from {expected_source}")

    bitmap_match = re.search(
        rf"static const uint8_t {stem}Bitmaps\[[^]]+\] = \{{(.*?)\n\}};",
        header,
        re.S,
    )
    glyph_match = re.search(
        rf"static const EpdGlyph {stem}Glyphs\[\] = \{{(.*?)\n\}};",
        header,
        re.S,
    )
    interval_match = re.search(
        rf"static const EpdUnicodeInterval {stem}Intervals\[\] = \{{(.*?)\n\}};",
        header,
        re.S,
    )
    if not bitmap_match or not glyph_match or not interval_match:
        raise RuntimeError(f"Unable to parse generated font header {path}")

    bitmap = bytes(int(value, 16) for value in re.findall(r"0x[0-9A-Fa-f]+", bitmap_match.group(1)))
    glyphs = []
    for values in re.findall(
        r"\{\s*\d+,\s*\d+,\s*\d+,\s*-?\d+,\s*-?\d+,\s*(\d+),\s*(\d+)\s*\}",
        glyph_match.group(1),
    ):
        length, offset = map(int, values)
        glyphs.append(bitmap[offset : offset + length])

    glyph_by_codepoint = {}
    for first, last, offset in re.findall(
        r"\{\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+)\s*\}",
        interval_match.group(1),
    ):
        first, last, offset = map(lambda value: int(value, 16), (first, last, offset))
        for codepoint in range(first, last + 1):
            glyph_by_codepoint[codepoint] = glyphs[offset + codepoint - first]

    covered = set(codepoints) & glyph_by_codepoint.keys()
    ink = sum(byte.bit_count() for codepoint in covered for byte in glyph_by_codepoint[codepoint])
    return ink, covered


def main() -> None:
    for label, family, reference_style, optical_style, codepoints, autohint in FACES:
        family_dir = SOURCE_DIR / family
        reference_path = family_dir / f"{family}-{reference_style}.ttf"
        optical_path = family_dir / f"{family}-{optical_style}.ttf"

        ratios = []
        for size in SIZES:
            reference_ink, reference_coverage = ink_pixels(reference_path, codepoints, size, False)
            optical_ink, optical_coverage = ink_pixels(optical_path, codepoints, size, True, autohint)
            if optical_coverage != reference_coverage:
                missing = sorted(reference_coverage - optical_coverage)
                raise RuntimeError(f"{label} is missing glyphs: {[f'U+{cp:04X}' for cp in missing]}")
            ratio = optical_ink / reference_ink
            if not MIN_INK_RATIO <= ratio <= MAX_INK_RATIO:
                raise RuntimeError(f"{label} {size}pt ink ratio is {ratio:.1%}")

            header_style = "medium" if optical_style == "UIMedium" else "bold"
            stem = f"ubuntu_{size}_{header_style}"
            header_path = SOURCE_DIR.parent / f"{stem}.h"
            header_ink, header_coverage = generated_header_ink(
                header_path,
                stem,
                optical_path.name,
                codepoints,
            )
            if header_coverage != optical_coverage:
                raise RuntimeError(f"{header_path} does not contain the expected {label} glyphs")
            if header_ink != optical_ink:
                raise RuntimeError(f"{header_path} does not contain the expected {label} bitmaps")
            ratios.append(ratio)

        mean_advance, max_advance = advance_deltas(reference_path, optical_path, codepoints)
        if abs(mean_advance) > MAX_MEAN_ADVANCE_DELTA:
            raise RuntimeError(f"{label} mean advance delta is {mean_advance:+.1%}")
        if max_advance > MAX_GLYPH_ADVANCE_DELTA:
            raise RuntimeError(f"{label} maximum advance delta is {max_advance:.1%}")

        print(
            f"{label}: ink {ratios[0]:.1%}/{ratios[1]:.1%}, "
            f"advance mean {mean_advance:+.1%}, max {max_advance:.1%}"
        )


if __name__ == "__main__":
    main()
