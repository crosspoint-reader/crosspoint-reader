#!/usr/bin/env python3
"""Validate generated GlyphStream font headers against bitmap dump ground truth."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

try:
    from .glyphstream import GlyphBitmap, GlyphStreamModel, decode_font, load_bitmap_dump, load_model_header
except ImportError:
    from glyphstream import GlyphBitmap, GlyphStreamModel, decode_font, load_bitmap_dump, load_model_header


@dataclass(frozen=True)
class ParsedGlyph:
    width: int
    height: int
    data_length: int
    data_offset: int
    codepoint: int


@dataclass(frozen=True)
class ParsedFont:
    name: str
    bitmap: bytes
    glyphs: tuple[ParsedGlyph, ...]
    is_2bit: bool
    bitmap_format: int


def _required_match(pattern: str, source: str, description: str) -> re.Match[str]:
    match = re.search(pattern, source, re.DOTALL)
    if match is None:
        raise ValueError(f"missing or malformed {description}")
    return match


def parse_font_header_text(source: str) -> ParsedFont:
    bitmap_match = _required_match(
        r"static const uint8_t\s+(\w+)Bitmaps\[(\d+)\]\s*=\s*\{(.*?)\};",
        source,
        "bitmap array",
    )
    name = bitmap_match.group(1)
    declared_bitmap_size = int(bitmap_match.group(2))
    bitmap = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", bitmap_match.group(3)))
    if len(bitmap) != declared_bitmap_size:
        raise ValueError(f"{name} bitmap array length differs from its declaration")

    glyph_match = _required_match(
        rf"static const EpdGlyph\s+{re.escape(name)}Glyphs\[\]\s*=\s*\{{(.*?)\}};",
        source,
        "glyph array",
    )
    raw_glyphs = [
        tuple(int(value) for value in match)
        for match in re.findall(
            r"\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}",
            glyph_match.group(1),
        )
    ]
    if not raw_glyphs:
        raise ValueError(f"{name} glyph array is empty")

    interval_match = _required_match(
        rf"static const EpdUnicodeInterval\s+{re.escape(name)}Intervals\[\]\s*=\s*\{{(.*?)\}};",
        source,
        "interval array",
    )
    intervals = [
        tuple(int(value, 0) for value in match)
        for match in re.findall(
            r"\{\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\}",
            interval_match.group(1),
        )
    ]
    codepoints: list[int | None] = [None] * len(raw_glyphs)
    for first, last, offset in intervals:
        if first > last or offset + last - first >= len(codepoints):
            raise ValueError(f"{name} interval exceeds glyph array")
        for codepoint in range(first, last + 1):
            glyph_index = offset + codepoint - first
            if codepoints[glyph_index] is not None:
                raise ValueError(f"{name} intervals overlap")
            codepoints[glyph_index] = codepoint
    if any(codepoint is None for codepoint in codepoints):
        raise ValueError(f"{name} intervals do not cover every glyph")

    font_match = _required_match(
        rf"static const EpdFontData\s+{re.escape(name)}\s*=\s*\{{(.*?)\}};",
        source,
        "font initializer",
    )
    initializer = re.sub(r"//.*", "", font_match.group(1))
    fields = [field.strip() for field in initializer.split(",") if field.strip()]
    if len(fields) != 24:
        raise ValueError(f"{name} EpdFontData initializer has {len(fields)} fields instead of 24")
    if fields[8] != "nullptr" or fields[9] != "0" or re.search(
        rf"static const EpdFontGroup\s+{re.escape(name)}Groups", source
    ):
        raise ValueError(f"{name} contains legacy compression groups")
    bitmap_format = int(fields[23], 0)
    if bitmap_format != 1:
        raise ValueError(f"{name} bitmapFormat is {bitmap_format}, expected 1")
    if fields[7] not in ("true", "false"):
        raise ValueError(f"{name} has invalid is2Bit value")
    is_2bit = fields[7] == "true"

    glyphs: list[ParsedGlyph] = []
    expected_offset = 0
    for glyph_index, (values, codepoint) in enumerate(zip(raw_glyphs, codepoints)):
        width, height, _, _, _, data_length, data_offset = values
        if data_offset != expected_offset:
            raise ValueError(
                f"{name} glyph {glyph_index} stream offset {data_offset} differs from expected {expected_offset}"
            )
        if (width == 0 or height == 0) and data_length != 0:
            raise ValueError(f"{name} empty glyph {glyph_index} has stream bytes")
        if width > 0 and height > 0 and data_length == 0:
            raise ValueError(f"{name} non-empty glyph {glyph_index} has no stream")
        expected_offset += data_length
        if expected_offset > len(bitmap):
            raise ValueError(f"{name} glyph {glyph_index} stream exceeds bitmap array")
        glyphs.append(ParsedGlyph(width, height, data_length, data_offset, int(codepoint)))
    if expected_offset != len(bitmap):
        raise ValueError(f"{name} bitmap array has unreferenced trailing bytes")

    return ParsedFont(name, bitmap, tuple(glyphs), is_2bit, bitmap_format)


def validate_parsed_font(
    parsed: ParsedFont,
    expected_glyphs: list[GlyphBitmap],
    is_2bit: bool,
    model: GlyphStreamModel,
) -> None:
    if parsed.is_2bit != is_2bit:
        raise ValueError(f"{parsed.name} bit depth differs from ground truth")
    if len(parsed.glyphs) != len(expected_glyphs):
        raise ValueError(f"{parsed.name} glyph count differs from ground truth")

    metadata: list[GlyphBitmap] = []
    streams: list[bytes] = []
    for glyph_index, (parsed_glyph, expected) in enumerate(zip(parsed.glyphs, expected_glyphs)):
        if (
            parsed_glyph.width != expected.width
            or parsed_glyph.height != expected.height
            or parsed_glyph.codepoint != expected.codepoint
        ):
            raise ValueError(f"{parsed.name} glyph {glyph_index} metadata differs from ground truth")
        metadata.append(
            GlyphBitmap(parsed_glyph.width, parsed_glyph.height, [], parsed_glyph.codepoint)
        )
        start = parsed_glyph.data_offset
        streams.append(parsed.bitmap[start : start + parsed_glyph.data_length])

    decoded = decode_font(metadata, streams, is_2bit, model)
    for glyph_index, (actual, expected) in enumerate(zip(decoded, expected_glyphs)):
        if actual.pixels != expected.pixels:
            raise ValueError(
                f"{parsed.name} glyph {glyph_index} U+{expected.codepoint:04X} pixel mismatch"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--headers", required=True, type=Path)
    parser.add_argument("--dumps", required=True, type=Path)
    args = parser.parse_args()

    model = load_model_header(args.model)
    dump_paths = sorted(args.dumps.glob("*.npz"))
    if not dump_paths:
        print(f"No bitmap dumps found under {args.dumps}", file=sys.stderr)
        return 1

    for dump_path in dump_paths:
        font_name, expected_glyphs, is_2bit = load_bitmap_dump(dump_path)
        header_path = args.headers / f"{font_name}.h"
        try:
            parsed = parse_font_header_text(header_path.read_text(encoding="utf-8"))
            if parsed.name != font_name:
                raise ValueError(f"header declares {parsed.name}, expected {font_name}")
            validate_parsed_font(parsed, expected_glyphs, is_2bit, model)
        except (OSError, ValueError) as error:
            print(f"FAIL: {font_name}: {error}", file=sys.stderr)
            return 1
        print(f"PASS: {font_name}: {len(expected_glyphs)} glyphs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
