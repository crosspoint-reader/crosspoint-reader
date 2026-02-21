#!python3
import freetype
import zlib
import sys
import re
import math
import argparse
from collections import namedtuple

# Originally from https://github.com/vroland/epdiy

parser = argparse.ArgumentParser(description="Generate a header file from a font to be used with epdiy.")
parser.add_argument("name", action="store", help="name of the font.")
parser.add_argument("size", type=int, help="font size to use.")
parser.add_argument("fontstack", action="store", nargs='+', help="list of font files, ordered by descending priority.")
parser.add_argument("--2bit", dest="is2Bit", action="store_true", help="generate 2-bit greyscale bitmap instead of 1-bit black and white.")
parser.add_argument("--additional-intervals", dest="additional_intervals", action="append", help="Additional code point intervals to export as min,max. This argument can be repeated.")
parser.add_argument("--compress", dest="compress", action="store_true", help="Compress glyph bitmaps using DEFLATE with group-based compression.")
parser.add_argument("--frequency-table", dest="frequency_table", help="TSV file with codepoint<TAB>rank for frequency-based CJK grouping.")
parser.add_argument("--group-size", dest="group_size", type=int, default=128, help="Glyphs per frequency group (default 128).")
parser.add_argument("--pin-groups", dest="pin_groups", type=int, default=0, help="Number of high-frequency groups to mark for pinning (default 0).")
parser.add_argument("--max-cjk-ideographs", dest="max_cjk_ideographs", type=int, default=0, help="Limit CJK Unified Ideographs (U+4E00-U+9FFF) to top N by frequency. 0 = no limit.")
parser.add_argument("--max-hangul", dest="max_hangul", type=int, default=0, help="Limit Hangul Syllables (U+AC00-U+D7AF) to top N by frequency. 0 = no limit.")
parser.add_argument("--non-pinned-group-size", dest="non_pinned_group_size", type=int, default=0, help="Group size for non-pinned CJK groups sorted by codepoint. 0 = use --group-size for all.")
args = parser.parse_args()

GlyphProps = namedtuple("GlyphProps", ["width", "height", "advance_x", "left", "top", "data_length", "data_offset", "code_point"])

font_stack = [freetype.Face(f) for f in args.fontstack]
is2Bit = args.is2Bit
size = args.size
font_name = args.name

# Load frequency table if provided
frequency_map = {}  # codepoint -> rank
if args.frequency_table:
    with open(args.frequency_table, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split('\t')
            if len(parts) >= 2:
                cp = int(parts[0], 0)
                rank = int(parts[1])
                frequency_map[cp] = rank

# Build allowed codepoint sets for CJK/Hangul filtering
allowed_cjk_ideographs = None
if args.max_cjk_ideographs > 0 and frequency_map:
    # Collect CJK Unified Ideographs from frequency table, sorted by rank
    cjk_entries = [(cp, rank) for cp, rank in frequency_map.items() if 0x4E00 <= cp <= 0x9FFF]
    cjk_entries.sort(key=lambda x: x[1])
    allowed_cjk_ideographs = set(cp for cp, _ in cjk_entries[:args.max_cjk_ideographs])
    print(f"// CJK ideograph limit: {args.max_cjk_ideographs} (from {len(cjk_entries)} in frequency table)", file=sys.stderr)

allowed_hangul = None
if args.max_hangul > 0 and frequency_map:
    hangul_entries = [(cp, rank) for cp, rank in frequency_map.items() if 0xAC00 <= cp <= 0xD7AF]
    hangul_entries.sort(key=lambda x: x[1])
    allowed_hangul = set(cp for cp, _ in hangul_entries[:args.max_hangul])
    print(f"// Hangul syllable limit: {args.max_hangul} (from {len(hangul_entries)} in frequency table)", file=sys.stderr)

# inclusive unicode code point intervals
# must not overlap and be in ascending order
intervals = [
    ### Basic Latin ###
    # ASCII letters, digits, punctuation, control characters
    (0x0020, 0x007F),
    ### Latin-1 Supplement ###
    # Accented characters for Western European languages
    (0x0080, 0x00FF),
    ### Latin Extended-A ###
    # Eastern European and Baltic languages
    (0x0100, 0x017F),
    ### General Punctuation (core subset) ###
    # Smart quotes, en dash, em dash, ellipsis, NO-BREAK SPACE
    (0x2000, 0x206F),
    ### Basic Symbols From "Latin-1 + Misc" ###
    # dashes, quotes, prime marks
    (0x2010, 0x203A),
    # misc punctuation
    (0x2040, 0x205F),
    # common currency symbols
    (0x20A0, 0x20CF),
    ### Combining Diacritical Marks (minimal subset) ###
    # Needed for proper rendering of many extended Latin languages
    (0x0300, 0x036F),
    ### Greek & Coptic ###
    # Used in science, maths, philosophy, some academic texts
    # (0x0370, 0x03FF),
    ### Cyrillic ###
    # Russian, Ukrainian, Bulgarian, etc.
    (0x0400, 0x04FF),
    ### Math Symbols (common subset) ###
    # Superscripts and Subscripts
    (0x2070, 0x209F),
    # General math operators
    (0x2200, 0x22FF),
    # Arrows
    (0x2190, 0x21FF),
    ### Specials
    # Replacement Character
    (0xFFFD, 0xFFFD),
]

add_ints = []
if args.additional_intervals:
    add_ints = [tuple([int(n, base=0) for n in i.split(",")]) for i in args.additional_intervals]

def norm_floor(val):
    return int(math.floor(val / (1 << 6)))

def norm_ceil(val):
    return int(math.ceil(val / (1 << 6)))

def chunks(l, n):
    for i in range(0, len(l), n):
        yield l[i:i + n]

def is_cjk_ideograph(cp):
    return 0x4E00 <= cp <= 0x9FFF

def is_hangul_syllable(cp):
    return 0xAC00 <= cp <= 0xD7AF

def should_include_codepoint(code_point):
    """Check if a codepoint should be included based on frequency filters."""
    if allowed_cjk_ideographs is not None and is_cjk_ideograph(code_point):
        return code_point in allowed_cjk_ideographs
    if allowed_hangul is not None and is_hangul_syllable(code_point):
        return code_point in allowed_hangul
    return True

def load_glyph(code_point):
    face_index = 0
    while face_index < len(font_stack):
        face = font_stack[face_index]
        glyph_index = face.get_char_index(code_point)
        if glyph_index > 0:
            face.load_glyph(glyph_index, freetype.FT_LOAD_RENDER)
            return face
        face_index += 1
    print(f"code point {code_point} ({hex(code_point)}) not found in font stack!", file=sys.stderr)
    return None

unmerged_intervals = sorted(intervals + add_ints)
intervals = []
unvalidated_intervals = []
for i_start, i_end in unmerged_intervals:
    if len(unvalidated_intervals) > 0 and i_start + 1 <= unvalidated_intervals[-1][1]:
        unvalidated_intervals[-1] = (unvalidated_intervals[-1][0], max(unvalidated_intervals[-1][1], i_end))
        continue
    unvalidated_intervals.append((i_start, i_end))

for i_start, i_end in unvalidated_intervals:
    start = i_start
    for code_point in range(i_start, i_end + 1):
        if not should_include_codepoint(code_point):
            if start < code_point:
                intervals.append((start, code_point - 1))
            start = code_point + 1
            continue
        face = load_glyph(code_point)
        if face is None:
            if start < code_point:
                intervals.append((start, code_point - 1))
            start = code_point + 1
    if start != i_end + 1:
        intervals.append((start, i_end))

for face in font_stack:
    face.set_char_size(size << 6, size << 6, 150, 150)

total_size = 0
all_glyphs = []

for i_start, i_end in intervals:
    for code_point in range(i_start, i_end + 1):
        face = load_glyph(code_point)
        bitmap = face.glyph.bitmap

        # Build out 4-bit greyscale bitmap
        pixels4g = []
        px = 0
        for i, v in enumerate(bitmap.buffer):
            y = i / bitmap.width
            x = i % bitmap.width
            if x % 2 == 0:
                px = (v >> 4)
            else:
                px = px | (v & 0xF0)
                pixels4g.append(px);
                px = 0
            # eol
            if x == bitmap.width - 1 and bitmap.width % 2 > 0:
                pixels4g.append(px)
                px = 0

        if is2Bit:
            # 0-3 white, 4-7 light grey, 8-11 dark grey, 12-15 black
            # Downsample to 2-bit bitmap
            pixels2b = []
            px = 0
            pitch = (bitmap.width // 2) + (bitmap.width % 2)
            for y in range(bitmap.rows):
                for x in range(bitmap.width):
                    px = px << 2
                    bm = pixels4g[y * pitch + (x // 2)]
                    bm = (bm >> ((x % 2) * 4)) & 0xF

                    if bm >= 12:
                        px += 3
                    elif bm >= 8:
                        px += 2
                    elif bm >= 4:
                        px += 1

                    if (y * bitmap.width + x) % 4 == 3:
                        pixels2b.append(px)
                        px = 0
            if (bitmap.width * bitmap.rows) % 4 != 0:
                px = px << (4 - (bitmap.width * bitmap.rows) % 4) * 2
                pixels2b.append(px)
        else:
            # Downsample to 1-bit bitmap - treat any 2+ as black
            pixelsbw = []
            px = 0
            pitch = (bitmap.width // 2) + (bitmap.width % 2)
            for y in range(bitmap.rows):
                for x in range(bitmap.width):
                    px = px << 1
                    bm = pixels4g[y * pitch + (x // 2)]
                    px += 1 if ((x & 1) == 0 and bm & 0xE > 0) or ((x & 1) == 1 and bm & 0xE0 > 0) else 0

                    if (y * bitmap.width + x) % 8 == 7:
                        pixelsbw.append(px)
                        px = 0
            if (bitmap.width * bitmap.rows) % 8 != 0:
                px = px << (8 - (bitmap.width * bitmap.rows) % 8)
                pixelsbw.append(px)

        pixels = pixels2b if is2Bit else pixelsbw

        # Build output data
        packed = bytes(pixels)
        glyph = GlyphProps(
            width = bitmap.width,
            height = bitmap.rows,
            advance_x = norm_floor(face.glyph.advance.x),
            left = face.glyph.bitmap_left,
            top = face.glyph.bitmap_top,
            data_length = len(packed),
            data_offset = total_size,
            code_point = code_point,
        )
        total_size += len(packed)
        all_glyphs.append((glyph, packed))

# pipe seems to be a good heuristic for the "real" descender
face = load_glyph(ord('|'))

glyph_data = []
glyph_props = []
for index, glyph in enumerate(all_glyphs):
    props, packed = glyph
    glyph_data.extend([b for b in packed])
    glyph_props.append(props)

compress = args.compress
use_frequency_grouping = compress and bool(args.frequency_table) and bool(frequency_map)


def to_byte_aligned(packed, width, height):
    """Convert packed 2-bit bitmap to byte-aligned format (rows padded to byte boundary).

    In packed format, pixels flow continuously across row boundaries (4 pixels/byte).
    In byte-aligned format, each row starts at a byte boundary, padding the last byte
    of each row with zero bits if width % 4 != 0. This improves DEFLATE compression
    because identical pixel rows produce identical byte patterns regardless of position.
    """
    if width == 0 or height == 0:
        return b''
    row_stride = (width + 3) // 4  # bytes per byte-aligned row
    aligned = bytearray(row_stride * height)
    for y in range(height):
        for x in range(width):
            # Read pixel from packed format (continuous bit stream)
            packed_pos = y * width + x
            packed_byte_idx = packed_pos // 4
            packed_shift = (3 - (packed_pos % 4)) * 2
            pixel = (packed[packed_byte_idx] >> packed_shift) & 0x3

            # Write pixel to byte-aligned format (row-aligned)
            aligned_byte_idx = y * row_stride + x // 4
            aligned_shift = (3 - (x % 4)) * 2
            aligned[aligned_byte_idx] |= (pixel << aligned_shift)
    return bytes(aligned)


# Build groups for compression
if compress and not is2Bit:
    print("Error: --compress requires --2bit (byte-aligned compression only supports 2-bit format)", file=sys.stderr)
    sys.exit(1)
glyphToGroup = None  # Will be set for frequency-grouped fonts

if compress:
    # Script-based grouping: glyphs that co-occur in typical text rendering
    # are grouped together for efficient LRU caching on the embedded target.
    SCRIPT_GROUP_RANGES = [
        (0x0000, 0x007F),   # ASCII
        (0x0080, 0x00FF),   # Latin-1 Supplement
        (0x0100, 0x017F),   # Latin Extended-A
        (0x0300, 0x036F),   # Combining Diacritical Marks
        (0x0400, 0x04FF),   # Cyrillic
        (0x2000, 0x206F),   # General Punctuation
        (0x2070, 0x209F),   # Superscripts & Subscripts
        (0x20A0, 0x20CF),   # Currency Symbols
        (0x2190, 0x21FF),   # Arrows
        (0x2200, 0x22FF),   # Math Operators
        (0xFFFD, 0xFFFD),   # Replacement Character
    ]

    # Threshold: codepoints >= this use frequency grouping (when enabled)
    CJK_FREQUENCY_THRESHOLD = 0x3000

    def get_script_group(code_point):
        for i, (start, end) in enumerate(SCRIPT_GROUP_RANGES):
            if start <= code_point <= end:
                return i
        return -1

    if use_frequency_grouping:
        # Hybrid grouping: Latin uses script-based, CJK uses frequency-based
        group_size = args.group_size
        pin_groups = args.pin_groups

        # Step 1: Build Latin groups (script-based, for codepoints < CJK_FREQUENCY_THRESHOLD)
        latin_groups = []  # list of lists of glyph indices
        current_group_id = None
        current_group = []

        for i, (props, packed) in enumerate(all_glyphs):
            if props.code_point >= CJK_FREQUENCY_THRESHOLD:
                continue
            sg = get_script_group(props.code_point)
            if sg != current_group_id:
                if current_group:
                    latin_groups.append(current_group)
                current_group_id = sg
                current_group = [i]
            else:
                current_group.append(i)

        if current_group:
            latin_groups.append(current_group)

        # Step 2: Build CJK groups
        # Collect CJK glyph indices with their frequency ranks
        cjk_glyphs_with_rank = []
        for i, (props, packed) in enumerate(all_glyphs):
            if props.code_point < CJK_FREQUENCY_THRESHOLD:
                continue
            rank = frequency_map.get(props.code_point, 999999)
            cjk_glyphs_with_rank.append((i, rank))

        # Sort by frequency rank (most frequent first)
        cjk_glyphs_with_rank.sort(key=lambda x: x[1])

        # Two-tier grouping:
        # - Pinned groups: frequency-sorted, group_size glyphs each
        # - Non-pinned groups: codepoint-sorted (Kangxi radical order for better
        #   compression), non_pinned_group_size glyphs each
        non_pinned_size = args.non_pinned_group_size if args.non_pinned_group_size > 0 else group_size

        # Build pinned groups (frequency-sorted, group_size each)
        pinned_count = pin_groups * group_size
        pinned_glyphs = cjk_glyphs_with_rank[:pinned_count]
        remaining_glyphs = cjk_glyphs_with_rank[pinned_count:]

        pinned_cjk_groups = []
        for bucket_start in range(0, len(pinned_glyphs), group_size):
            bucket = pinned_glyphs[bucket_start:bucket_start + group_size]
            group_indices = sorted([gi for gi, _ in bucket])
            pinned_cjk_groups.append(group_indices)

        # Build non-pinned groups (kept in frequency order for cache locality)
        remaining_cjk_groups = []
        for bucket_start in range(0, len(remaining_glyphs), non_pinned_size):
            bucket = remaining_glyphs[bucket_start:bucket_start + non_pinned_size]
            group_indices = sorted([gi for gi, _ in bucket])
            remaining_cjk_groups.append(group_indices)

        # Step 3: Combine groups: pinned CJK groups first, then Latin, then remaining CJK
        # Pinned groups go first so they get group indices 0..pin_groups-1
        all_groups = pinned_cjk_groups + latin_groups + remaining_cjk_groups

        # Build glyphToGroup mapping
        total_glyph_count = len(all_glyphs)
        glyphToGroup = [0] * total_glyph_count
        for group_idx, glyph_indices in enumerate(all_groups):
            for gi in glyph_indices:
                glyphToGroup[gi] = group_idx

        # Build compressed data for each group
        compressed_groups = []
        compressed_bitmap_data = []
        modified_glyph_props = list(glyph_props)

        for group_idx, glyph_indices in enumerate(all_groups):
            group_packed = b''
            group_aligned = b''
            for gi in glyph_indices:
                props, packed = all_glyphs[gi]
                within_group_offset = len(group_packed)
                old_props = modified_glyph_props[gi]
                modified_glyph_props[gi] = GlyphProps(
                    width=old_props.width,
                    height=old_props.height,
                    advance_x=old_props.advance_x,
                    left=old_props.left,
                    top=old_props.top,
                    data_length=old_props.data_length,
                    data_offset=within_group_offset,
                    code_point=old_props.code_point,
                )
                group_packed += packed
                group_aligned += to_byte_aligned(packed, old_props.width, old_props.height)

            compressor = zlib.compressobj(level=9, wbits=-15)
            compressed = compressor.compress(group_aligned) + compressor.flush()

            # For frequency-grouped fonts, firstGlyphIndex is set to the first member for informational purposes
            first_glyph_in_group = glyph_indices[0] if glyph_indices else 0
            compressed_groups.append((compressed, len(group_aligned), len(glyph_indices), first_glyph_in_group))
            compressed_bitmap_data.extend(compressed)

        glyph_props = modified_glyph_props
        total_compressed = len(compressed_bitmap_data)
        total_packed = len(glyph_data)

        cjk_count = sum(1 for _, (p, _) in enumerate(all_glyphs) if p.code_point >= CJK_FREQUENCY_THRESHOLD)
        print(f"// Compression: {total_packed} packed -> {total_compressed} compressed ({100*total_compressed/total_packed:.1f}%)", file=sys.stderr)
        print(f"// Groups: {len(all_groups)} ({len(pinned_cjk_groups)} pinned CJK [{group_size}/grp] + {len(latin_groups)} Latin + {len(remaining_cjk_groups)} CJK [{non_pinned_size}/grp])", file=sys.stderr)
        print(f"// Glyphs: {len(all_glyphs)} total ({cjk_count} CJK, {len(all_glyphs) - cjk_count} Latin)", file=sys.stderr)

    else:
        # Pure script-based grouping (no frequency table)
        groups = []  # list of (first_glyph_index, glyph_count)
        current_group_id = None
        group_start = 0
        group_count = 0

        for i, (props, packed) in enumerate(all_glyphs):
            sg = get_script_group(props.code_point)
            if sg != current_group_id:
                if group_count > 0:
                    groups.append((group_start, group_count))
                current_group_id = sg
                group_start = i
                group_count = 1
            else:
                group_count += 1

        if group_count > 0:
            groups.append((group_start, group_count))

        # Compress each group
        compressed_groups = []
        compressed_bitmap_data = []
        compressed_offset = 0

        modified_glyph_props = list(glyph_props)

        for first_idx, count in groups:
            group_packed = b''
            group_aligned = b''
            for gi in range(first_idx, first_idx + count):
                props, packed = all_glyphs[gi]
                within_group_offset = len(group_packed)
                old_props = modified_glyph_props[gi]
                modified_glyph_props[gi] = GlyphProps(
                    width=old_props.width,
                    height=old_props.height,
                    advance_x=old_props.advance_x,
                    left=old_props.left,
                    top=old_props.top,
                    data_length=old_props.data_length,
                    data_offset=within_group_offset,
                    code_point=old_props.code_point,
                )
                group_packed += packed
                group_aligned += to_byte_aligned(packed, old_props.width, old_props.height)

            compressor = zlib.compressobj(level=9, wbits=-15)
            compressed = compressor.compress(group_aligned) + compressor.flush()

            compressed_groups.append((compressed, len(group_aligned), count, first_idx))
            compressed_bitmap_data.extend(compressed)
            compressed_offset += len(compressed)

        glyph_props = modified_glyph_props
        total_compressed = len(compressed_bitmap_data)
        total_packed = len(glyph_data)
        print(f"// Compression: {total_packed} packed -> {total_compressed} compressed ({100*total_compressed/total_packed:.1f}%), {len(groups)} groups", file=sys.stderr)

print(f"""/**
 * generated by fontconvert.py
 * name: {font_name}
 * size: {size}
 * mode: {'2-bit' if is2Bit else '1-bit'}{'  compressed: true' if compress else ''}
 * Command used: {' '.join(sys.argv)}
 */
#pragma once
#include "EpdFontData.h"
""")

if compress:
    print(f"static const uint8_t {font_name}Bitmaps[{len(compressed_bitmap_data)}] = {{")
    for c in chunks(compressed_bitmap_data, 16):
        print ("    " + " ".join(f"0x{b:02X}," for b in c))
    print ("};\n");
else:
    print(f"static const uint8_t {font_name}Bitmaps[{len(glyph_data)}] = {{")
    for c in chunks(glyph_data, 16):
        print ("    " + " ".join(f"0x{b:02X}," for b in c))
    print ("};\n");

print(f"static const EpdGlyph {font_name}Glyphs[] = {{")
for i, g in enumerate(glyph_props):
    print ("    { " + ", ".join([f"{a}" for a in list(g[:-1])]),"},", f"// {chr(g.code_point) if g.code_point != 92 else '<backslash>'}")
print ("};\n");

print(f"static const EpdUnicodeInterval {font_name}Intervals[] = {{")
offset = 0
for i_start, i_end in intervals:
    print (f"    {{ 0x{i_start:X}, 0x{i_end:X}, 0x{offset:X} }},")
    offset += i_end - i_start + 1
print ("};\n");

if compress:
    print(f"static const EpdFontGroup {font_name}Groups[] = {{")
    compressed_offset = 0
    for compressed, uncompressed_size, count, first_idx in compressed_groups:
        print(f"    {{ {compressed_offset}, {len(compressed)}, {uncompressed_size}, {count}, {first_idx} }},")
        compressed_offset += len(compressed)
    print("};\n")

# Emit glyphToGroup array for frequency-grouped fonts
if glyphToGroup is not None:
    print(f"static const uint16_t {font_name}GlyphToGroup[] = {{")
    for c in chunks(glyphToGroup, 32):
        print("    " + " ".join(f"{v}," for v in c))
    print("};\n")

print(f"static const EpdFontData {font_name} = {{")
print(f"    {font_name}Bitmaps,")
print(f"    {font_name}Glyphs,")
print(f"    {font_name}Intervals,")
print(f"    {len(intervals)},")
print(f"    {norm_ceil(face.size.height)},")
print(f"    {norm_ceil(face.size.ascender)},")
print(f"    {norm_floor(face.size.descender)},")
print(f"    {'true' if is2Bit else 'false'},")
if compress:
    print(f"    {font_name}Groups,")
    print(f"    {len(compressed_groups)},")
else:
    print("    nullptr,")
    print("    0,")
# glyphToGroup
if glyphToGroup is not None:
    print(f"    {font_name}GlyphToGroup,")
else:
    print("    nullptr,")
print("};")
