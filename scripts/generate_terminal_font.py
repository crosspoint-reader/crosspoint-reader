#!/usr/bin/env python3
"""Generate a compact knietty 8x16 lookup table from a BDF font."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Glyph:
    codepoint: int
    width: int
    height: int
    x_offset: int
    y_offset: int
    bitmap: tuple[int, ...]


def parse_bdf(path: Path) -> tuple[list[Glyph], int]:
    glyphs: list[Glyph] = []
    font_baseline = 12
    codepoint: int | None = None
    bounds: tuple[int, int, int, int] | None = None
    bitmap: list[int] | None = None

    for raw_line in path.read_text(encoding="ascii").splitlines():
        line = raw_line.strip()
        if line.startswith("FONTBOUNDINGBOX "):
            _, height, _, y_offset = map(int, line.split()[1:])
            font_baseline = height + y_offset
        elif line.startswith("ENCODING "):
            codepoint = int(line.split()[1])
        elif line.startswith("BBX "):
            width, height, x_offset, y_offset = map(int, line.split()[1:])
            bounds = width, height, x_offset, y_offset
        elif line == "BITMAP":
            bitmap = []
        elif line == "ENDCHAR":
            if codepoint is not None and bounds is not None and bitmap is not None and 0 <= codepoint <= 0xFFFF:
                glyphs.append(Glyph(codepoint, *bounds, tuple(bitmap)))
            codepoint = None
            bounds = None
            bitmap = None
        elif bitmap is not None:
            bitmap.append(int(line, 16))

    return glyphs, font_baseline


def rasterize(glyph: Glyph, font_baseline: int) -> tuple[int, ...]:
    rows = [0] * 16
    for source_y, source_bits in enumerate(glyph.bitmap[: glyph.height]):
        target_y = font_baseline - glyph.y_offset - glyph.height + source_y
        if not 0 <= target_y < 16:
            continue
        for source_x in range(glyph.width):
            source_mask = 1 << (glyph.width - source_x - 1)
            target_x = glyph.x_offset + source_x
            if source_bits & source_mask and 0 <= target_x < 8:
                rows[target_y] |= 1 << (7 - target_x)
    return tuple(rows)


def codepoints_from_header(path: Path) -> set[int]:
    import re

    return {int(value, 16) for value in re.findall(r"\{0x([0-9a-fA-F]{4}), \{", path.read_text())}


def write_header(glyphs: list[Glyph], font_baseline: int, output: Path, source_name: str, license_note: str) -> None:
    lines = [
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        f"// Generated from {source_name} by scripts/generate_terminal_font.py.",
        f"// {license_note}",
        "namespace TerminalFontData {",
        "struct Glyph {",
        "  uint16_t codepoint;",
        "  uint8_t rows[16];",
        "};",
        "",
        "static constexpr Glyph GLYPHS[] = {",
    ]
    for glyph in sorted(glyphs, key=lambda item: item.codepoint):
        rows = ", ".join(f"0x{row:02x}" for row in rasterize(glyph, font_baseline))
        lines.append(f"    {{0x{glyph.codepoint:04x}, {{{rows}}}}},")
    lines.extend(
        [
            "};",
            "static constexpr uint16_t GLYPH_COUNT = sizeof(GLYPHS) / sizeof(GLYPHS[0]);",
            "static_assert(sizeof(Glyph) == 18);",
            "}  // namespace TerminalFontData",
            "",
        ]
    )
    output.write_text("\n".join(lines), encoding="ascii")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("bdf", type=Path)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("src/terminal/TerminalFontData.generated.h"),
    )
    parser.add_argument("--source-name", default="8x16 BDF font")
    parser.add_argument("--license-note", default="See the font license files distributed with knietty.")
    parser.add_argument(
        "--subset-from",
        type=Path,
        help="include only codepoints present in an existing generated header",
    )
    parser.add_argument(
        "--exclude-wide",
        action="store_true",
        help="drop glyphs whose BDF bounding box is wider than one 8-pixel cell",
    )
    args = parser.parse_args()
    glyphs, font_baseline = parse_bdf(args.bdf)
    if args.subset_from:
        subset = codepoints_from_header(args.subset_from)
        glyphs = [glyph for glyph in glyphs if glyph.codepoint in subset]
    # Unifont is duospaced. Its 16-pixel glyphs require a future wcwidth-aware
    # screen model and must not be silently cropped. Existing Spleen generation
    # intentionally preserves its few overhanging bounding boxes, so filtering
    # is opt-in rather than a global behavior change.
    if args.exclude_wide:
        glyphs = [glyph for glyph in glyphs if glyph.width <= 8]
    write_header(glyphs, font_baseline, args.output, args.source_name, args.license_note)


if __name__ == "__main__":
    main()
