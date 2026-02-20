#!/usr/bin/env python3
"""
Round-trip verification for compressed font headers.

Parses each generated .h file in the given directory, identifies compressed fonts
(those with a Groups array), decompresses each group, and verifies that
decompression succeeds and all glyph offsets/lengths fall within bounds.
"""
import os
import re
import sys
import zlib


def parse_hex_array(text):
    """Extract bytes from a C hex array string like '{ 0xAB, 0xCD, ... }'"""
    hex_vals = re.findall(r'0x([0-9A-Fa-f]{2})', text)
    return bytes(int(h, 16) for h in hex_vals)


def parse_groups(text):
    """Parse EpdFontGroup array entries: { compressedOffset, compressedSize, uncompressedSize, glyphCount, firstGlyphIndex }"""
    groups = []
    for match in re.finditer(r'\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', text):
        groups.append({
            'compressedOffset': int(match.group(1)),
            'compressedSize': int(match.group(2)),
            'uncompressedSize': int(match.group(3)),
            'glyphCount': int(match.group(4)),
            'firstGlyphIndex': int(match.group(5)),
        })
    return groups


def parse_glyphs(text):
    """Parse EpdGlyph array entries: { width, height, advanceX, left, top, dataLength, dataOffset }"""
    glyphs = []
    for match in re.finditer(r'\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}', text):
        glyphs.append({
            'width': int(match.group(1)),
            'height': int(match.group(2)),
            'advanceX': int(match.group(3)),
            'left': int(match.group(4)),
            'top': int(match.group(5)),
            'dataLength': int(match.group(6)),
            'dataOffset': int(match.group(7)),
        })
    return glyphs


def verify_font_file(filepath):
    """Verify a single font header file. Returns (font_name, success, message)."""
    with open(filepath, 'r') as f:
        content = f.read()

    # Check if this is a compressed font (has Groups array)
    groups_match = re.search(r'static const EpdFontGroup (\w+)Groups\[\]', content)
    if not groups_match:
        return (os.path.basename(filepath), None, "uncompressed, skipping")

    font_name = groups_match.group(1)

    # Extract bitmap data
    bitmap_match = re.search(
        r'static const uint8_t ' + re.escape(font_name) + r'Bitmaps\[\d+\]\s*=\s*\{([^}]+)\}',
        content, re.DOTALL
    )
    if not bitmap_match:
        return (font_name, False, "could not find Bitmaps array")

    compressed_data = parse_hex_array(bitmap_match.group(1))

    # Extract groups
    groups_array_match = re.search(
        r'static const EpdFontGroup ' + re.escape(font_name) + r'Groups\[\]\s*=\s*\{(.+?)\};',
        content, re.DOTALL
    )
    if not groups_array_match:
        return (font_name, False, "could not find Groups array")

    groups = parse_groups(groups_array_match.group(1))
    if not groups:
        return (font_name, False, "Groups array parsed to 0 entries; check format")

    # Extract glyphs
    glyphs_match = re.search(
        r'static const EpdGlyph ' + re.escape(font_name) + r'Glyphs\[\]\s*=\s*\{(.+?)\};',
        content, re.DOTALL
    )
    if not glyphs_match:
        return (font_name, False, "could not find Glyphs array")

    glyphs = parse_glyphs(glyphs_match.group(1))

    # Verify each group
    for gi, group in enumerate(groups):
        # Extract compressed chunk
        chunk = compressed_data[group['compressedOffset']:group['compressedOffset'] + group['compressedSize']]
        if len(chunk) != group['compressedSize']:
            return (font_name, False, f"group {gi}: compressed data truncated (expected {group['compressedSize']}, got {len(chunk)})")

        # Decompress with raw DEFLATE
        try:
            decompressed = zlib.decompress(chunk, -15)
        except zlib.error as e:
            return (font_name, False, f"group {gi}: decompression failed: {e}")

        if len(decompressed) != group['uncompressedSize']:
            return (font_name, False, f"group {gi}: size mismatch (expected {group['uncompressedSize']}, got {len(decompressed)})")

        # Verify each glyph's data within the group
        first = group['firstGlyphIndex']
        for j in range(group['glyphCount']):
            glyph_idx = first + j
            if glyph_idx >= len(glyphs):
                return (font_name, False, f"group {gi}: glyph index {glyph_idx} out of range")

            glyph = glyphs[glyph_idx]
            offset = glyph['dataOffset']
            length = glyph['dataLength']

            if offset + length > len(decompressed):
                return (font_name, False, f"group {gi}, glyph {glyph_idx}: data extends beyond decompressed buffer "
                        f"(offset={offset}, length={length}, decompressed_size={len(decompressed)})")

    return (font_name, True, f"{len(groups)} groups, {len(glyphs)} glyphs OK")


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <font_headers_directory>", file=sys.stderr)
        sys.exit(1)

    font_dir = sys.argv[1]
    if not os.path.isdir(font_dir):
        print(f"Error: {font_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    files = sorted(f for f in os.listdir(font_dir) if f.endswith('.h') and f != 'all.h')
    passed = 0
    failed = 0
    skipped = 0

    for filename in files:
        filepath = os.path.join(font_dir, filename)
        _font_name, success, message = verify_font_file(filepath)

        if success is None:
            skipped += 1
        elif success:
            passed += 1
            print(f"  PASS: {filename} ({message})")
        else:
            failed += 1
            print(f"  FAIL: {filename} - {message}")

    print(f"\nResults: {passed} passed, {failed} failed, {skipped} skipped (uncompressed)")

    if failed > 0:
        sys.exit(1)


if __name__ == '__main__':
    main()
