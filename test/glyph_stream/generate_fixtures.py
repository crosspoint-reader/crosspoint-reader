#!/usr/bin/env python3
"""Generate committed C++ fixtures for the GlyphStream decoder tests."""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from lib.EpdFont.scripts.glyphstream import (
    GlyphBitmap,
    GlyphStreamModel,
    Reference,
    decode_font,
    encode_font,
    load_model_header,
)


DEFAULT_OUTPUT = Path(__file__).with_name("GlyphStreamFixtures.generated.h")
MODEL_HEADER = REPO_ROOT / "lib/EpdFont/builtinFonts/glyphStreamModel.h"


@dataclass(frozen=True)
class Fixture:
    cpp_name: str
    is_2bit: bool
    glyphs: tuple[GlyphBitmap, ...]
    expected_references: tuple[Reference, ...]


def glyph_from_rows(codepoint: int, rows: str) -> GlyphBitmap:
    lines = tuple(line.strip() for line in rows.strip().splitlines())
    width = len(lines[0])
    if any(len(line) != width for line in lines):
        raise ValueError("glyph rows have inconsistent widths")
    pixels = [int(pixel) for line in lines for pixel in line]
    if any(pixel < 0 or pixel > 3 for pixel in pixels):
        raise ValueError("glyph rows contain a pixel outside 0..3")
    return GlyphBitmap(width, len(lines), pixels, codepoint)


DEPTH_TWO_TWO_BIT = Fixture(
    cpp_name="DepthTwoTwoBit",
    is_2bit=True,
    glyphs=(
        glyph_from_rows(
            ord("b"),
            """
            03332000000000
            03332000000000
            03332000000000
            03332000000000
            03332000000000
            03332023331000
            03332333333300
            03333333333320
            03333200133330
            03333000023331
            03332000013331
            03332000003332
            03332000003332
            03332000013332
            03333000023331
            03333100033330
            03333333333320
            03332333333300
            03330133333000
            00000000000000
            """,
        ),
        glyph_from_rows(
            ord("c"),
            """
            03332000000000
            03332000000000
            03332000000000
            03332000000000
            03332000000000
            03332013332000
            03332333333310
            03333333333330
            03333310133331
            03333000023331
            03333000013331
            03332000013331
            03332000013331
            03332000013331
            03332000013331
            03332000013331
            03332000013331
            03332000013331
            03332000013331
            """,
        ),
        glyph_from_rows(
            ord("d"),
            """
            02220013332000
            03330333333310
            03333333333330
            03333310133331
            03333000023331
            03333000013331
            03332000013331
            03332000013331
            03332000013331
            03332000013331
            03332000013331
            03332000013331
            03332000013331
            03332000013331
            """,
        ),
    ),
    expected_references=(Reference(), Reference(0, 0, 1), Reference(1, 5, 2)),
)

MAXIMUM_DIMENSIONS_ONE_BIT = Fixture(
    cpp_name="MaximumDimensionsOneBit",
    is_2bit=False,
    glyphs=(GlyphBitmap(63, 46, [0] * (63 * 46), ord("A")),),
    expected_references=(Reference(),),
)

FIXTURES = (DEPTH_TWO_TWO_BIT, MAXIMUM_DIMENSIONS_ONE_BIT)


def pack_expected_pixels(pixels: list[int], is_2bit: bool) -> bytes:
    """Pack fixture pixels independently from the production encoder helper."""
    bits_per_pixel = 2 if is_2bit else 1
    mask = 0x03 if is_2bit else 0x01
    packed = bytearray((len(pixels) * bits_per_pixel + 7) // 8)
    for index, pixel in enumerate(pixels):
        bit_index = index * bits_per_pixel
        packed[bit_index // 8] |= (pixel & mask) << (8 - bits_per_pixel - bit_index % 8)
    return bytes(packed)


def append_byte_array(lines: list[str], name: str, values: bytes) -> None:
    lines.append(f"inline constexpr std::array<std::uint8_t, {len(values)}> {name} = {{{{")
    for start in range(0, len(values), 12):
        chunk = ", ".join(f"0x{value:02x}" for value in values[start : start + 12])
        lines.append(f"    {chunk},")
    lines.append("}};")
    lines.append("")


def append_uint16_array(lines: list[str], name: str, values: list[int]) -> None:
    rendered = ", ".join(str(value) for value in values)
    lines.append(f"inline constexpr std::array<std::uint16_t, {len(values)}> {name} = {{{{{rendered}}}}};")
    lines.append("")


def append_fixture(lines: list[str], fixture: Fixture, model: GlyphStreamModel) -> None:
    streams, references = encode_font(list(fixture.glyphs), fixture.is_2bit, model)
    if tuple(references) != fixture.expected_references:
        raise ValueError(f"{fixture.cpp_name} no longer has the expected reference chain")
    if decode_font(list(fixture.glyphs), streams, fixture.is_2bit, model) != list(fixture.glyphs):
        raise ValueError(f"{fixture.cpp_name} failed Python encoder/decoder roundtrip")

    codepoints = [glyph.codepoint for glyph in fixture.glyphs]
    if codepoints != list(range(codepoints[0], codepoints[0] + len(codepoints))):
        raise ValueError(f"{fixture.cpp_name} codepoints must form one interval")

    stream_offsets: list[int] = []
    bitmap = bytearray()
    for stream in streams:
        stream_offsets.append(len(bitmap))
        bitmap.extend(stream)

    expected_offsets: list[int] = []
    expected_lengths: list[int] = []
    expected_bitmap = bytearray()
    for glyph in fixture.glyphs:
        expected = pack_expected_pixels(glyph.pixels, fixture.is_2bit)
        expected_offsets.append(len(expected_bitmap))
        expected_lengths.append(len(expected))
        expected_bitmap.extend(expected)

    prefix = f"k{fixture.cpp_name}"
    append_byte_array(lines, f"{prefix}Bitmap", bytes(bitmap))

    lines.append(
        f"inline constexpr std::array<EpdGlyph, {len(fixture.glyphs)}> {prefix}Glyphs = {{{{"
    )
    for glyph, stream, offset in zip(fixture.glyphs, streams, stream_offsets):
        lines.append(
            f"    {{ {glyph.width}, {glyph.height}, 0, 0, 0, {len(stream)}, {offset} }},"
        )
    lines.append("}};")
    lines.append("")

    lines.append(f"inline constexpr std::array<EpdUnicodeInterval, 1> {prefix}Intervals = {{{{")
    lines.append(f"    {{ 0x{codepoints[0]:x}, 0x{codepoints[-1]:x}, 0 }},")
    lines.append("}};")
    lines.append("")

    append_byte_array(lines, f"{prefix}ExpectedBitmap", bytes(expected_bitmap))
    append_uint16_array(lines, f"{prefix}ExpectedOffsets", expected_offsets)
    append_uint16_array(lines, f"{prefix}ExpectedLengths", expected_lengths)


def render_header() -> str:
    model = load_model_header(MODEL_HEADER)
    lines = [
        "// Generated by test/glyph_stream/generate_fixtures.py.",
        "// Regenerate: python3 test/glyph_stream/generate_fixtures.py",
        "// Do not edit by hand.",
        "",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        '#include "EpdFontData.h"',
        "",
        "namespace glyph_stream_fixtures {",
        "",
    ]
    for fixture in FIXTURES:
        append_fixture(lines, fixture, model)
    lines.extend(("}  // namespace glyph_stream_fixtures", ""))
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if the output is missing or stale")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help="generated header path")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    generated = render_header()
    if args.check:
        try:
            current = args.output.read_text(encoding="utf-8")
        except FileNotFoundError:
            current = ""
        if current != generated:
            print(
                f"{args.output} is out of date; run: python3 test/glyph_stream/generate_fixtures.py",
                file=sys.stderr,
            )
            return 1
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generated, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
