#!python3
import freetype
import zlib
import sys
import re
import os
import math
import struct
import argparse
from collections import namedtuple

# Originally from https://github.com/vroland/epdiy

# ── Binary format constants ──────────────────────────────────────────────────
# Partition image: [Header 20B] [GroupDir] [FontDir] [data …]
# Per-font .fontbin: [FontBinHeader 72B] [bitmap] [pad] [glyphs] [intervals]

PARTITION_MAGIC   = 0x43504654   # "CPFT" little-endian
PARTITION_VERSION = 2
MMU_PAGE_SIZE     = 0x10000      # 64KB alignment for esp_partition_mmap

# struct FontDirectoryEntry (68 bytes, little-endian, matches ESP32-C3 ABI)
#   char     name[32]
#   uint32_t bitmapOffset, bitmapSize
#   uint32_t glyphOffset, glyphCount
#   uint32_t intervalOffset, intervalCount
#   uint8_t  advanceY, is2Bit, _pad[2]
#   int32_t  ascender, descender
DIR_ENTRY_FMT  = '<32s IIIIII BB xx ii'
DIR_ENTRY_SIZE = struct.calcsize(DIR_ENTRY_FMT)   # 68

# struct GroupDirectoryEntry (24 bytes)
#   char     name[16]
#   uint32_t dataOffset   (64KB-aligned offset within partition)
#   uint32_t dataSize     (bytes of font data in this group)
GROUP_ENTRY_FMT  = '<16s II'
GROUP_ENTRY_SIZE = struct.calcsize(GROUP_ENTRY_FMT)   # 24

# struct EpdGlyph (16 bytes on ESP32-C3 RISC-V 32-bit)
#   uint8_t  width, height, advanceX  (+ 1 pad byte)
#   int16_t  left, top
#   uint16_t dataLength  (+ 2 pad bytes)
#   uint32_t dataOffset
GLYPH_FMT  = '<BBBx hh H xx I'
GLYPH_SIZE = struct.calcsize(GLYPH_FMT)   # 16

# struct EpdUnicodeInterval (12 bytes)
#   uint32_t first, last, offset
INTERVAL_FMT  = '<III'
INTERVAL_SIZE = struct.calcsize(INTERVAL_FMT)   # 12

# Per-font binary header (72 bytes)
#   char name[32], char group[16], uint32_t bitmap_size, glyph_count, interval_count,
#   uint8_t advance_y, is_2bit, pad[2], int32_t ascender, descender
FONTBIN_HDR_FMT  = '<32s 16s III BB xx ii'
FONTBIN_HDR_SIZE = struct.calcsize(FONTBIN_HDR_FMT)   # 72


def align4(n):
    return (n + 3) & ~3

def align_mmu(n):
    return (n + MMU_PAGE_SIZE - 1) & ~(MMU_PAGE_SIZE - 1)


# ── Pack mode ────────────────────────────────────────────────────────────────
def pack_fonts(bindir, output):
    """Combine .fontbin files into a grouped partition image."""
    fontbin_files = sorted(f for f in os.listdir(bindir) if f.endswith('.fontbin'))
    if not fontbin_files:
        print(f"No .fontbin files found in {bindir}", file=sys.stderr)
        sys.exit(1)

    fonts = []
    for fname in fontbin_files:
        path = os.path.join(bindir, fname)
        with open(path, 'rb') as f:
            hdr = f.read(FONTBIN_HDR_SIZE)
            name, group, bmp_sz, glyph_cnt, interval_cnt, adv_y, is2b, asc, desc = \
                struct.unpack(FONTBIN_HDR_FMT, hdr)
            rest = f.read()

        fonts.append({
            'name': name,
            'group': group.rstrip(b'\x00').decode('utf-8'),
            'bitmap_size': bmp_sz,
            'glyph_count': glyph_cnt,
            'interval_count': interval_cnt,
            'advance_y': adv_y,
            'is_2bit': is2b,
            'ascender': asc,
            'descender': desc,
            'data': rest,
        })

    # Group fonts: "ui" first, then remaining groups alphabetically
    from collections import OrderedDict
    groups = OrderedDict()
    for font in fonts:
        groups.setdefault(font['group'], []).append(font)
    sorted_group_names = sorted(groups.keys(), key=lambda g: (0 if g == 'ui' else 1, g))
    groups = OrderedDict((g, groups[g]) for g in sorted_group_names)

    font_count = len(fonts)
    group_count = len(groups)

    # Header: 20 bytes + group directory + font directory
    header_size = 20 + group_count * GROUP_ENTRY_SIZE + font_count * DIR_ENTRY_SIZE
    first_data_offset = align_mmu(header_size)

    # Lay out each group's font data at 64KB-aligned boundaries
    cursor = first_data_offset
    group_layouts = OrderedDict()    # group_name -> (data_offset, fonts_with_offsets)
    flat_entries = []                # (font, bmp_off, glyph_off, interval_off) in group order

    for group_name in groups:
        group_data_start = cursor
        for font in groups[group_name]:
            bmp_off = cursor
            cursor += align4(font['bitmap_size'])
            glyph_off = cursor
            cursor += font['glyph_count'] * GLYPH_SIZE
            interval_off = cursor
            cursor += font['interval_count'] * INTERVAL_SIZE
            cursor = align4(cursor)
            flat_entries.append((font, bmp_off, glyph_off, interval_off))
        group_data_size = cursor - group_data_start
        group_layouts[group_name] = (group_data_start, group_data_size)
        cursor = align_mmu(cursor)

    data_size = cursor

    # Write output
    with open(output, 'wb') as out:
        # Partition header (20 bytes)
        out.write(struct.pack('<IIIII', PARTITION_MAGIC, PARTITION_VERSION,
                              font_count, data_size, group_count))

        # Group directory
        for group_name, (g_offset, g_size) in group_layouts.items():
            name_bytes = group_name.encode('utf-8')[:16].ljust(16, b'\x00')
            out.write(struct.pack(GROUP_ENTRY_FMT, name_bytes, g_offset, g_size))

        # Font directory (in group order)
        for font, bmp_off, glyph_off, interval_off in flat_entries:
            out.write(struct.pack(DIR_ENTRY_FMT,
                font['name'],
                bmp_off, font['bitmap_size'],
                glyph_off, font['glyph_count'],
                interval_off, font['interval_count'],
                font['advance_y'], font['is_2bit'],
                font['ascender'], font['descender'],
            ))

        # Write font data for each group
        for font, bmp_off, glyph_off, interval_off in flat_entries:
            pad_to = bmp_off - out.tell()
            if pad_to > 0:
                out.write(b'\x00' * pad_to)
            out.write(font['data'])

        # Pad to final alignment
        remainder = out.tell() % 4
        if remainder:
            out.write(b'\x00' * (4 - remainder))

    total = os.path.getsize(output)
    print(f"Packed {font_count} fonts in {group_count} groups into {output} ({total} bytes, {total/1024:.1f} KB)")
    for group_name, (g_offset, g_size) in group_layouts.items():
        n = len(groups[group_name])
        print(f"  {group_name}: {n} fonts, offset=0x{g_offset:X}, size={g_size} bytes ({g_size/1024:.1f} KB)")


# ── Check for --pack-fonts mode before normal argparse ───────────────────────
if len(sys.argv) > 1 and sys.argv[1] == '--pack-fonts':
    if len(sys.argv) != 4:
        print("Usage: fontconvert.py --pack-fonts <bindir> <output.bin>", file=sys.stderr)
        sys.exit(1)
    pack_fonts(sys.argv[2], sys.argv[3])
    sys.exit(0)

# ── Normal single-font mode ─────────────────────────────────────────────────
parser = argparse.ArgumentParser(description="Generate a header file from a font to be used with epdiy.")
parser.add_argument("name", action="store", help="name of the font.")
parser.add_argument("size", type=int, help="font size to use.")
parser.add_argument("fontstack", action="store", nargs='+', help="list of font files, ordered by descending priority.")
parser.add_argument("--2bit", dest="is2Bit", action="store_true", help="generate 2-bit greyscale bitmap instead of 1-bit black and white.")
parser.add_argument("--additional-intervals", dest="additional_intervals", action="append", help="Additional code point intervals to export as min,max. This argument can be repeated.")
parser.add_argument("--binary-out", dest="binary_out", metavar="FILE", help="Write binary .fontbin file instead of C header.")
parser.add_argument("--group", dest="group", default="ui", help="Font group name for binary output (e.g. ui, bookerly, notosans, opendyslexic).")
args = parser.parse_args()

GlyphProps = namedtuple("GlyphProps", ["width", "height", "advance_x", "left", "top", "data_length", "data_offset", "code_point"])

font_stack = [freetype.Face(f) for f in args.fontstack]
is2Bit = args.is2Bit
size = args.size
font_name = args.name

# inclusive unicode code point intervals
# must not overlap and be in ascending order
intervals = [
    ### Basic Latin ###
    # ASCII letters, digits, punctuation, control characters
    (0x0000, 0x007F),
    ### Latin-1 Supplement ###
    # Accented characters for Western European languages
    (0x0080, 0x00FF),
    ### Latin Extended-A ###
    # Eastern European and Baltic languages
    (0x0100, 0x017F),
    ### Latin Extended-B ###
    # Latin letters for Eastern European and African languages
    (0x0180, 0x024F),
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
    ### Letterlike Symbols ###
    # ™ ℃ № ℓ Ω etc., common in non-fiction
    (0x2100, 0x214F),
    ### Combining Diacritical Marks (minimal subset) ###
    # Needed for proper rendering of many extended Latin languages
    (0x0300, 0x036F),
    ### Greek & Coptic ###
    # Used in science, maths, philosophy, some academic texts
    (0x0370, 0x03FF),
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
    ### CJK ###
    # Core Unified Ideographs
    # (0x4E00, 0x9FFF),
    # # Extension A
    # (0x3400, 0x4DBF),
    # # Extension B
    # (0x20000, 0x2A6DF),
    # # Extension C–F
    # (0x2A700, 0x2EBEF),
    # # Extension G
    # (0x30000, 0x3134F),
    # # Hiragana
    # (0x3040, 0x309F),
    # # Katakana
    # (0x30A0, 0x30FF),
    # # Katakana Phonetic Extensions
    # (0x31F0, 0x31FF),
    # # Halfwidth Katakana
    # (0xFF60, 0xFF9F),
    # # Hangul Syllables
    # (0xAC00, 0xD7AF),
    # # Hangul Jamo
    # (0x1100, 0x11FF),
    # # Hangul Compatibility Jamo
    # (0x3130, 0x318F),
    # # Hangul Jamo Extended-A
    # (0xA960, 0xA97F),
    # # Hangul Jamo Extended-B
    # (0xD7B0, 0xD7FF),
    # # CJK Radicals Supplement
    # (0x2E80, 0x2EFF),
    # # Kangxi Radicals
    # (0x2F00, 0x2FDF),
    # # CJK Symbols and Punctuation
    # (0x3000, 0x303F),
    # # CJK Compatibility Forms
    # (0xFE30, 0xFE4F),
    # # CJK Compatibility Ideographs
    # (0xF900, 0xFAFF),
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

def load_glyph(code_point):
    face_index = 0
    while face_index < len(font_stack):
        face = font_stack[face_index]
        glyph_index = face.get_char_index(code_point)
        if glyph_index > 0:
            face.load_glyph(glyph_index, freetype.FT_LOAD_RENDER)
            return face
        face_index += 1
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

            # for y in range(bitmap.rows):
            #     line = ''
            #     for x in range(bitmap.width):
            #         pixelPosition = y * bitmap.width + x
            #         byte = pixels2b[pixelPosition // 4]
            #         bit_index = (3 - (pixelPosition % 4)) * 2
            #         line += '#' if ((byte >> bit_index) & 3) > 0 else '.'
            #     print(line)
            # print('')
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

            # for y in range(bitmap.rows):
            #     line = ''
            #     for x in range(bitmap.width):
            #         pixelPosition = y * bitmap.width + x
            #         byte = pixelsbw[pixelPosition // 8]
            #         bit_index = 7 - (pixelPosition % 8)
            #         line += '#' if (byte >> bit_index) & 1 else '.'
            #     print(line)
            # print('')

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

advance_y = norm_ceil(face.size.height)
ascender  = norm_ceil(face.size.ascender)
descender = norm_floor(face.size.descender)

# ── Binary output mode ───────────────────────────────────────────────────────
if args.binary_out:
    bitmap_bytes = bytes(glyph_data)
    bitmap_size  = len(bitmap_bytes)
    glyph_count  = len(glyph_props)
    interval_count = len(intervals)

    # Serialize glyphs as packed structs matching ESP32-C3 EpdGlyph layout
    glyph_bin = b''
    for g in glyph_props:
        glyph_bin += struct.pack(GLYPH_FMT,
            g.width, g.height, g.advance_x,
            g.left, g.top,
            g.data_length,
            g.data_offset)

    # Serialize intervals
    interval_bin = b''
    offset = 0
    for i_start, i_end in intervals:
        interval_bin += struct.pack(INTERVAL_FMT, i_start, i_end, offset)
        offset += i_end - i_start + 1

    # Write .fontbin: header + bitmap (padded to 4B) + glyphs + intervals
    name_bytes = font_name.encode('utf-8')[:32].ljust(32, b'\x00')
    group_bytes = args.group.encode('utf-8')[:16].ljust(16, b'\x00')
    hdr = struct.pack(FONTBIN_HDR_FMT,
        name_bytes, group_bytes,
        bitmap_size, glyph_count, interval_count,
        advance_y, 1 if is2Bit else 0,
        ascender, descender)

    bitmap_padded = bitmap_bytes + b'\x00' * (align4(bitmap_size) - bitmap_size)

    with open(args.binary_out, 'wb') as f:
        f.write(hdr)
        f.write(bitmap_padded)
        f.write(glyph_bin)
        f.write(interval_bin)

    total = FONTBIN_HDR_SIZE + len(bitmap_padded) + len(glyph_bin) + len(interval_bin)
    print(f"  {font_name}: {total} bytes ({bitmap_size} bitmap, {glyph_count} glyphs, {interval_count} intervals)", file=sys.stderr)
    sys.exit(0)

# ── C header output (original behaviour) ─────────────────────────────────────
print(f"""/**
 * generated by fontconvert.py
 * name: {font_name}
 * size: {size}
 * mode: {'2-bit' if is2Bit else '1-bit'}
 * Command used: {' '.join(sys.argv)}
 */
#pragma once
#include "EpdFontData.h"
""")

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

print(f"static const EpdFontData {font_name} = {{")
print(f"    {font_name}Bitmaps,")
print(f"    {font_name}Glyphs,")
print(f"    {font_name}Intervals,")
print(f"    {len(intervals)},")
print(f"    {advance_y},")
print(f"    {ascender},")
print(f"    {descender},")
print(f"    {'true' if is2Bit else 'false'},")
print("};")
