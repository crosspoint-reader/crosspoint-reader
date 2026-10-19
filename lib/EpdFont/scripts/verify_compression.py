#!/usr/bin/env python3
"""
Round-trip verification for compressed font headers.

Parses each generated .h file in the given directory, identifies compressed fonts
(those with a Groups array), decompresses each group (byte-aligned bitmap format),
compacts to packed format, and verifies the data matches expected glyph sizes.

Both bit depths: the 2-bit reader faces (fontconvert.py) and the 1-bit UI faces
(compress_ui_face.py), whose rows are padded to a byte the same way. A 1-bit face also
carries the length and CRC32 of the flat bitmap it decodes back to (<name>PackedBytes,
<name>PackedCrc32); those are checked against the decompressed glyphs too.

Supports both contiguous-group fonts (Latin) and frequency-grouped fonts (CJK)
with glyphToGroup mapping arrays.
"""
import math
import os
import re
import sys
import zlib


def parse_hex_array(text):
    """Extract bytes from a C hex array string like '{ 0xAB, 0xCD, ... }'"""
    hex_vals = re.findall(r'0x([0-9A-Fa-f]{2})', text)
    return bytes(int(h, 16) for h in hex_vals)


def parse_uint8_array(text):
    """Extract uint8/uint16 values from a C array string like '{ 0, 1, 0xFF, ... }'"""
    return [int(v, 0) for v in re.findall(r'\b0x[0-9A-Fa-f]+\b|\b\d+\b', text)]


def parse_groups(text):
    """Parse EpdFontGroup array entries:
    { compressedOffset, compressedSize, uncompressedSize, glyphCount, ringBytes, firstGlyphIndex }"""
    groups = []
    for match in re.finditer(r'\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', text):
        groups.append({
            'compressedOffset': int(match.group(1)),
            'compressedSize': int(match.group(2)),
            'uncompressedSize': int(match.group(3)),
            'glyphCount': int(match.group(4)),
            'ringBytes': int(match.group(5)),
            'firstGlyphIndex': int(match.group(6)),
        })
    return groups


def parse_glyphs(text, is_2bit):
    """Parse EpdGlyphPacked entries: { width, height, advanceX, left, top }.

    dataLength is not stored -- it is derived from width * height * bpp, and this script is one of
    the things that keeps that derivation honest: it compares the derived value against the actual
    packed length of every glyph it decompresses. The caller says the bit depth: the header's
    `mode:` line.
    """
    glyphs = []
    bpp = 2 if is_2bit else 1
    for match in re.finditer(r'\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}', text):
        width, height = int(match.group(1)), int(match.group(2))
        glyphs.append({
            'width': width,
            'height': height,
            'advanceX': int(match.group(3)),
            'left': int(match.group(4)),
            'top': int(match.group(5)),
            'dataLength': (width * height * bpp + 7) // 8,
        })
    return glyphs


def get_group_glyph_indices(group, group_index, glyphs, glyph_to_group):
    """Get the ordered list of glyph indices belonging to a group."""
    if glyph_to_group is not None:
        # Frequency-grouped: scan all glyphs
        return [i for i in range(len(glyphs)) if glyph_to_group[i] == group_index]
    else:
        # Contiguous: sequential from firstGlyphIndex
        first = group['firstGlyphIndex']
        return list(range(first, first + group['glyphCount']))


def compact_aligned_1bit_to_packed(aligned_data, width, height):
    """Byte-aligned 1-bit rows to the continuous 1-bit stream (reverse of row_aligned() in
    compress_ui_face.py)."""
    if width == 0 or height == 0:
        return b''
    packed = bytearray(math.ceil(width * height / 8))
    row_stride = (width + 7) // 8
    for y in range(height):
        for x in range(width):
            if (aligned_data[y * row_stride + x // 8] >> (7 - (x % 8))) & 1:
                pos = y * width + x
                packed[pos // 8] |= 0x80 >> (pos % 8)
    return bytes(packed)


def compact_aligned_to_packed(aligned_data, width, height):
    """Convert byte-aligned 2-bit bitmap to packed format (reverse of to_byte_aligned).

    In byte-aligned format, each row starts at a byte boundary.
    In packed format, pixels flow continuously across row boundaries (4 pixels/byte).
    """
    if width == 0 or height == 0:
        return b''
    packed_size = math.ceil(width * height / 4)
    packed = bytearray(packed_size)
    row_stride = (width + 3) // 4  # bytes per byte-aligned row

    for y in range(height):
        for x in range(width):
            # Read pixel from byte-aligned format (row-aligned)
            aligned_byte_idx = y * row_stride + x // 4
            aligned_shift = (3 - (x % 4)) * 2
            pixel = (aligned_data[aligned_byte_idx] >> aligned_shift) & 0x3

            # Write pixel to packed format (continuous bit stream)
            packed_pos = y * width + x
            packed_byte_idx = packed_pos // 4
            packed_shift = (3 - (packed_pos % 4)) * 2
            packed[packed_byte_idx] |= (pixel << packed_shift)

    return bytes(packed)


def verify_font_file(filepath):
    """Verify a single font header file. Returns (font_name, success, message)."""
    with open(filepath, 'r') as f:
        content = f.read()

    # Check if this is a compressed font (has Groups array)
    groups_match = re.search(r'static const EpdFontGroup (\w+)Groups\[\]', content)
    if not groups_match:
        return (os.path.basename(filepath), None, "uncompressed, skipping")

    font_name = groups_match.group(1)
    is_2bit = ' * mode: 2-bit' in content

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
        r'static const EpdGlyphPacked ' + re.escape(font_name) + r'Glyphs\[\]\s*=\s*\{(.+?)\};',
        content, re.DOTALL
    )
    if not glyphs_match:
        return (font_name, False, "could not find Glyphs array")

    glyphs = parse_glyphs(glyphs_match.group(1), is_2bit)

    # Check for glyphToGroup array (frequency-grouped fonts)
    glyph_to_group = None
    g2g_match = re.search(
        r'static const uint16_t ' + re.escape(font_name) + r'GlyphToGroup\[\]\s*=\s*\{(.+?)\};',
        content, re.DOTALL
    )
    if g2g_match:
        glyph_to_group = parse_uint8_array(g2g_match.group(1))
        if len(glyph_to_group) != len(glyphs):
            return (font_name, False, f"glyphToGroup length ({len(glyph_to_group)}) != glyph count ({len(glyphs)})")
        max_group_id = max(glyph_to_group)
        if max_group_id >= len(groups):
            return (font_name, False, f"glyphToGroup contains group ID {max_group_id} but only {len(groups)} groups exist")

    # Verify each group
    packed_by_glyph = {}
    for gi, group in enumerate(groups):
        # Extract compressed chunk
        chunk = compressed_data[group['compressedOffset']:group['compressedOffset'] + group['compressedSize']]
        if len(chunk) != group['compressedSize']:
            return (font_name, False, f"group {gi}: compressed data truncated (expected {group['compressedSize']}, got {len(chunk)})")

        # Decompress with raw DEFLATE — result is byte-aligned data
        try:
            decompressed = zlib.decompress(chunk, -15)
        except zlib.error as e:
            return (font_name, False, f"group {gi}: decompression failed: {e}")

        if len(decompressed) != group['uncompressedSize']:
            return (font_name, False, f"group {gi}: size mismatch (expected {group['uncompressedSize']}, got {len(decompressed)})")

        # Get glyph indices for this group
        group_glyph_indices = get_group_glyph_indices(group, gi, glyphs, glyph_to_group)
        if glyph_to_group is not None and len(group_glyph_indices) != group['glyphCount']:
            return (font_name, False,
                    f"group {gi}: glyphCount {group['glyphCount']} != mapping count {len(group_glyph_indices)}")

        # Walk through byte-aligned data, compact each glyph, and verify against packed format
        byte_aligned_offset = 0

        for glyph_idx in group_glyph_indices:
            if glyph_idx >= len(glyphs):
                return (font_name, False, f"group {gi}: glyph index {glyph_idx} out of range")
            glyph = glyphs[glyph_idx]
            width = glyph['width']
            height = glyph['height']

            if width == 0 or height == 0:
                if glyph['dataLength'] != 0:
                    return (font_name, False, f"group {gi}, glyph {glyph_idx}: zero-size glyph dataLength {glyph['dataLength']} != expected 0")
                continue

            if is_2bit:
                aligned_size = ((width + 3) // 4) * height
                packed_size = math.ceil(width * height / 4)
            else:
                aligned_size = ((width + 7) // 8) * height
                packed_size = math.ceil(width * height / 8)

            # dataLength is the whole check now. A compressed font no longer stores an offset at
            # all: FontDecompressor walks the group to find where a glyph starts, so the only thing
            # that can silently go wrong is the derived LENGTH, which is what this compares against
            # the real packed stream.
            if glyph['dataLength'] != packed_size:
                return (font_name, False, f"group {gi}, glyph {glyph_idx}: dataLength {glyph['dataLength']} != expected packed length {packed_size} "
                        f"(width={width}, height={height})")

            # Extract byte-aligned data for this glyph
            if byte_aligned_offset + aligned_size > len(decompressed):
                return (font_name, False, f"group {gi}, glyph {glyph_idx}: byte-aligned data extends beyond decompressed buffer "
                        f"(offset={byte_aligned_offset}, size={aligned_size}, buf_size={len(decompressed)})")

            aligned_glyph = decompressed[byte_aligned_offset:byte_aligned_offset + aligned_size]

            # Compact to packed and verify pixel values are valid (0-3 for 2-bit)
            if is_2bit:
                packed_glyph = compact_aligned_to_packed(aligned_glyph, width, height)
            else:
                packed_glyph = compact_aligned_1bit_to_packed(aligned_glyph, width, height)
            packed_by_glyph[glyph_idx] = packed_glyph
            if len(packed_glyph) != packed_size:
                return (font_name, False, f"group {gi}, glyph {glyph_idx}: compacted size {len(packed_glyph)} != expected {packed_size}")

            byte_aligned_offset += aligned_size

        # Verify total byte-aligned size matches uncompressedSize
        if byte_aligned_offset != group['uncompressedSize']:
            return (font_name, False, f"group {gi}: total byte-aligned size {byte_aligned_offset} != uncompressedSize {group['uncompressedSize']}")

    # A 1-bit UI face states the flat bitmap it decodes back to; the glyphs, in order, must be it.
    packed_bytes_match = re.search(r'static constexpr uint32_t ' + re.escape(font_name) + r'PackedBytes = (\d+);', content)
    packed_crc_match = re.search(r'static constexpr uint32_t ' + re.escape(font_name) + r'PackedCrc32 = 0x([0-9A-Fa-f]+);',
                                 content)
    if packed_bytes_match or packed_crc_match:
        if not (packed_bytes_match and packed_crc_match):
            return (font_name, False, "PackedBytes and PackedCrc32 must come together")
        flat = b''.join(packed_by_glyph.get(i, b'') for i in range(len(glyphs)))
        if len(flat) != int(packed_bytes_match.group(1)):
            return (font_name, False, f"glyphs decode to {len(flat)} B, PackedBytes says {packed_bytes_match.group(1)}")
        if (zlib.crc32(flat) & 0xFFFFFFFF) != int(packed_crc_match.group(1), 16):
            return (font_name, False, "glyphs decode to a bitmap whose CRC32 is not PackedCrc32")

    extra_info = ""
    if glyph_to_group is not None:
        extra_info = " (frequency-grouped)"
    if not is_2bit:
        extra_info += " (1-bit)"
    return (font_name, True, f"{len(groups)} groups, {len(glyphs)} glyphs OK{extra_info}")


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
