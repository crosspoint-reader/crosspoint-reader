#!python3
import freetype
import zlib
import sys
import re
import math
import argparse
from collections import namedtuple
from fontTools.ttLib import TTFont

from kern_extract import extract_raw_kerning
from kern_reduce import reduce_kern_classes

# Force UTF-8 stdout so that `> file.h` on Windows doesn't produce UTF-16 LE
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')

# Originally from https://github.com/vroland/epdiy

parser = argparse.ArgumentParser(description="Generate a header file from a font to be used with epdiy.")
parser.add_argument("name", action="store", help="name of the font.")
parser.add_argument("size", type=int, help="font size to use.")
parser.add_argument("fontstack", action="store", nargs='+', help="list of font files, ordered by descending priority.")
parser.add_argument("--2bit", dest="is2Bit", action="store_true", help="generate 2-bit greyscale bitmap instead of 1-bit black and white.")
parser.add_argument("--additional-intervals", dest="additional_intervals", action="append", help="Additional code point intervals to export as min,max. This argument can be repeated.")
parser.add_argument("--compress", dest="compress", action="store_true", help="Compress glyph bitmaps using DEFLATE with group-based compression.")
parser.add_argument("--zopfli", dest="zopfli", action="store_true", help="Use Zopfli for the DEFLATE backend instead of zlib. Produces standard raw-DEFLATE streams (decoded unchanged by the on-device uzlib inflater), typically a few percent smaller than zlib -9, at the cost of much slower compression. Requires --compress and the 'zopfli' package.")
parser.add_argument("--threshold", dest="threshold", type=float, default=0.4, help="Coverage threshold (0-1) for 1-bit black/white quantisation: pixels with greyscale coverage >= threshold become black. Lower = bolder stems. Default 0.4. Ignored for --2bit.")
args = parser.parse_args()

GlyphProps = namedtuple("GlyphProps", ["width", "height", "advance_x", "left", "top", "data_length", "data_offset", "code_point"])

# Must match FontDecompressor::HOT_GLYPH_BUF_SIZE. A glyph that packs larger than this cannot be
# served by the device's per-glyph fallback cache and renders blank whenever the page prewarm
# misses it, so the check below refuses to generate one.
HOT_GLYPH_BUF_SIZE = 640

font_stack = [freetype.Face(f) for f in args.fontstack]
is2Bit = args.is2Bit
size = args.size
font_name = args.name
threshold = args.threshold
# Always rasterise through FreeType's auto-hinter rather than the font's own
# TrueType hints, because it grid-snaps stems to whole pixels. Both output modes
# need that, for the same underlying reason: a stem that is not grid-fitted lands
# at whatever subpixel phase the outline dictates, and the quantisation step
# afterwards freezes that phase into the font.
#
#   1-bit: native mono rasterising left single-pixel stems spindly and uneven,
#          while the old un-hinted >=~13% threshold over-inked them. Rendering
#          antialiased and thresholding a hinted coverage map gives evenly
#          weighted, solid stems at small UI sizes.
#   2-bit: un-hinted, a stem centred on a pixel quantises to (black,black,black)
#          while the same stem straddling a boundary quantises to
#          (light,black,black,light) — a 3px vs 4px ink footprint on otherwise
#          identical letters. At Bookerly 14 that split 'b d f i n r u' from the
#          rest of the alphabet and read as inconsistent letter thickness
#          (issue #149). Hinting collapses every stem to one width.
#
# Regenerating an existing font with this on does not reflow text: advanceX comes
# from linearHoriAdvance (unhinted) below, so only the ink boxes move, never the pen.
load_flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_FORCE_AUTOHINT

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
    ### Latin Extended-B (Vietnamese subset only) ###
    # Only Ơ/ơ (U+01A0-01A1), Ư/ư (U+01AF-01B0) for Vietnamese
    (0x01A0, 0x01A1),
    (0x01AF, 0x01B0),
    ### Latin Extended-B (European subset only) ###
    # Croatian digraphs (DŽ/Lj/Nj), Pinyin caron variants,
    # European diacritical variants, Romanian (Ș/ș/Ț/ț)
    (0x01C4, 0x021F),
    ### Vietnamese Extended ###
    # All precomposed Vietnamese characters with tone marks
    # Ả Ấ Ầ Ẩ Ẫ Ậ Ắ Ằ Ẳ Ẵ Ặ Ẹ Ẻ Ẽ Ế Ề Ể Ễ Ệ Ỉ Ị Ọ Ỏ Ố Ồ Ổ Ỗ Ộ Ớ Ờ Ở Ỡ Ợ Ụ Ủ Ứ Ừ Ử Ữ Ự Ỳ Ỵ Ỷ Ỹ
    (0x1EA0, 0x1EF9),
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
    ### Alphabetic Presentation Forms (Latin ligatures) ###
    # ff, fi, fl, ffi, ffl, long-st, st
    (0xFB00, 0xFB06),
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

# Fixed-point (fp4) output conventions (must match EpdFontData.h / fp4 namespace):
#
#   advanceX    12.4 unsigned fixed-point (uint16_t).
#               12 integer bits, 4 fractional bits = 1/16-pixel resolution.
#               Encoded from FreeType's 16.16 linearHoriAdvance.
#
#   kernMatrix  4.4 signed fixed-point (int8_t).
#               4 integer bits, 4 fractional bits = 1/16-pixel resolution.
#               Range: -8.0 to +7.9375 pixels.
#               Encoded from font design-unit kerning values.
#
# Both share 4 fractional bits so the renderer can add them directly into a
# single int32_t accumulator and defer rounding until pixel placement.

def fp4_from_ft16_16(val):
    """Convert FreeType 16.16 fixed-point to 12.4 fixed-point with rounding."""
    return (val + (1 << 11)) >> 12

def fp4_from_design_units(du, scale):
    """Convert a font design-unit value to 4.4 fixed-point, clamped to int8_t.

    Multiplies by scale (ppem / units_per_em) and shifts into 4 fractional
    bits.  The result is rounded to nearest and clamped to [-128, 127].
    """
    raw = round(du * scale * 16)
    return max(-128, min(127, raw))

# Unicode Default_Ignorable_Code_Point ranges (BMP), which must never produce ink.
#
# These are formatting controls: the text layer acts on them, the renderer must not draw
# them. Fonts do not agree about that. Bookerly maps U+200C..U+200F to real outlines
# (gid 1855-1858, verified with FreeType against Bookerly-Regular.ttf), so load_glyph()
# below accepted them, FT_LOAD_RENDER rasterised them, and advance_x came from
# linearHoriAdvance -- which is correctly 0. The result was a glyph that PAINTS AND DOES
# NOT ADVANCE THE PEN, so the next character overprints it. 41 of 46 generated builtin
# font headers carried at least one such glyph (e.g. bookerly_14_regular U+200C: 2x24 px,
# advance 0). Books watermarked with zero-width steganography turn that into thousands of
# stacked marks per chapter.
#
# Two rules, both required:
#   1. never rasterise one   -- emit a 0x0 / advance-0 / no-bitmap entry instead
#   2. never drop one from an interval -- EpdFont::getGlyph() falls back to
#      REPLACEMENT_GLYPH (U+FFFD) for an uncovered codepoint, which would swap invisible
#      ink for a visible box. They must stay in the interval table AS empty glyphs.
#
# U+00AD SOFT HYPHEN is deliberately NOT here: it is Default_Ignorable, but this firmware
# breaks lines on it and must draw a hyphen when it does.
DEFAULT_IGNORABLE_RANGES = (
    (0x034F, 0x034F),  # combining grapheme joiner
    (0x061C, 0x061C),  # Arabic letter mark
    (0x115F, 0x1160),  # Hangul choseong/jungseong fillers
    (0x17B4, 0x17B5),  # Khmer inherent vowels
    (0x180B, 0x180F),  # Mongolian variation selectors + FVS
    (0x200B, 0x200F),  # ZWSP, ZWNJ, ZWJ, LRM, RLM
    (0x202A, 0x202E),  # bidi embedding/override
    (0x2060, 0x2064),  # word joiner, invisible operators
    (0x2065, 0x2065),  # unassigned, reserved ignorable
    (0x206A, 0x206F),  # deprecated format characters
    (0x3164, 0x3164),  # Hangul filler
    (0xFE00, 0xFE0F),  # variation selectors 1-16
    (0xFEFF, 0xFEFF),  # zero-width no-break space / BOM
    (0xFFA0, 0xFFA0),  # halfwidth Hangul filler
    (0xFFF0, 0xFFF8),  # unassigned, reserved ignorable
)

def is_default_ignorable(code_point):
    """True for codepoints that must render as nothing, whatever the font says."""
    for lo, hi in DEFAULT_IGNORABLE_RANGES:
        if lo <= code_point <= hi:
            return True
        if code_point < lo:
            break
    return False

def chunks(l, n):
    for i in range(0, len(l), n):
        yield l[i:i + n]

def load_glyph(code_point):
    face_index = 0
    while face_index < len(font_stack):
        face = font_stack[face_index]
        glyph_index = face.get_char_index(code_point)
        if glyph_index > 0:
            face.load_glyph(glyph_index, load_flags)
            return face
        face_index += 1
    return None

def shipped_level(coverage):
    """The ink level an 8-bit FreeType coverage value ships as. Mirrors the two quantisers in the
    glyph loop: the 1-bit threshold, and the 2-bit path's 4-bit downsample (which keeps
    coverage >> 4) followed by its 4/8/12 cuts."""
    if not is2Bit:
        return 1 if coverage >= int(round(threshold * 255)) else 0
    nibble = coverage >> 4
    return 3 if nibble >= 12 else 2 if nibble >= 8 else 1 if nibble >= 4 else 0

def ink_centre(slot, level=lambda coverage: coverage):
    """Horizontal centre of a rendered glyph's ink, in pixels from the pen, each pixel weighted by
    level(coverage). None when nothing is inked."""
    bitmap = slot.bitmap
    pitch = abs(bitmap.pitch)
    total = moment = 0
    for y in range(bitmap.rows):
        for x in range(bitmap.width):
            weight = level(bitmap.buffer[y * pitch + x])
            total += weight
            moment += weight * (x + 0.5)
    return slot.bitmap_left + moment / total if total else None

def unhinted_ink_centre(code_point):
    """ink_centre() of the glyph as designed, before the auto-hinter moves it. Picks the face the
    same way load_glyph() does, and must run BEFORE load_glyph(): both load into the one slot."""
    for face in font_stack:
        glyph_index = face.get_char_index(code_point)
        if glyph_index > 0:
            face.load_glyph(glyph_index, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_NO_HINTING)
            return ink_centre(face.glyph)
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
        # Keep default-ignorables in the interval even when the font has no cmap entry for
        # them: a gap here sends getGlyph() to REPLACEMENT_GLYPH (U+FFFD) and draws a box
        # where the text layer expects nothing. They are emitted as empty glyphs below.
        if is_default_ignorable(code_point):
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
recentred_glyphs = 0

for i_start, i_end in intervals:
    for code_point in range(i_start, i_end + 1):
        # Formatting controls never carry ink. Emit the empty glyph directly rather than
        # asking FreeType, which happily rasterises whatever outline the font maps them to
        # (see DEFAULT_IGNORABLE_RANGES). advance_x 0 is what these codepoints already
        # report via linearHoriAdvance, so nothing about line layout changes -- only the
        # bitmap goes away. That also means regenerating a font with this fix in place
        # cannot repaginate a book.
        if is_default_ignorable(code_point):
            all_glyphs.append((GlyphProps(
                width = 0,
                height = 0,
                advance_x = 0,
                left = 0,
                top = 0,
                data_length = 0,
                data_offset = total_size,
                code_point = code_point,
            ), bytes()))
            continue

        designed_centre = unhinted_ink_centre(code_point)
        face = load_glyph(code_point)
        bitmap = face.glyph.bitmap

        # Put the ink back where the design has it. The auto-hinter moves a glyph sideways as
        # well as snapping its stems -- it rounds the glyph's origin to a whole pixel, reporting
        # the rounding in lsb_delta/rsb_delta for a renderer to add back, and the stems then snap
        # on top of that. Our renderer places glyphs on the UNHINTED advance and never sees
        # those deltas, so the drift stays in the bitmap. Inter 12's "i" landed 0.85 px right
        # of its design position, which the threshold froze into a whole pixel: a visible gap
        # before every "i" and none after it. Compare the centre of the ink as it ships with
        # the centre of the unhinted outline and move `left` by the whole-pixel part of the
        # difference. Stem snapping is untouched, and so is the advance, so nothing reflows.
        shipped_centre = ink_centre(face.glyph, shipped_level)
        ink_shift = 0
        if designed_centre is not None and shipped_centre is not None:
            ink_shift = math.floor(shipped_centre - designed_centre + 0.5)
        if ink_shift:
            recentred_glyphs += 1

        if is2Bit:
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
                    pixels4g.append(px)
                    px = 0
                # eol
                if x == bitmap.width - 1 and bitmap.width % 2 > 0:
                    pixels4g.append(px)
                    px = 0

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
            # 1-bit: FreeType rasterised an 8-bit greyscale coverage map (auto-
            # hinted). Threshold each pixel to black/white and repack it into the
            # firmware's continuous (non-row-padded) 1-bit bitstream.
            pixelsbw = []
            px = 0
            src_pitch = abs(bitmap.pitch)
            cutoff = int(round(threshold * 255))
            for y in range(bitmap.rows):
                for x in range(bitmap.width):
                    bit = 1 if bitmap.buffer[y * src_pitch + x] >= cutoff else 0
                    px = (px << 1) | bit

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
            # We use linearHoriAdvance (16.16 fixed-point, unhinted) instead of
            # advance.x (26.6 fixed-point, grid-fitted to whole pixels by hinter)
            advance_x = fp4_from_ft16_16(face.glyph.linearHoriAdvance),
            left = face.glyph.bitmap_left - ink_shift,
            top = face.glyph.bitmap_top,
            data_length = len(packed),
            data_offset = total_size,
            code_point = code_point,
        )
        total_size += len(packed)

        # The on-device fallback path (FontDecompressor::getBitmap, taken when a glyph missed
        # the page prewarm) compacts a glyph into a fixed FallbackSlot buffer of
        # HOT_GLYPH_BUF_SIZE bytes and gives up if it does not fit -- so an oversized glyph does
        # not fail here, it silently renders BLANK on the device. Catching it at generation time
        # is the only place it is visible. Went unnoticed until 2026-08-19, when exactly one
        # glyph in the built-in set (bookerly_18_bolditalic 54x38) was found to overflow a
        # 512-byte buffer by a single byte.
        if len(packed) > HOT_GLYPH_BUF_SIZE:
            print(f"Error: glyph U+{code_point:04X} ({bitmap.width}x{bitmap.rows}) packs to {len(packed)} bytes, "
                  f"over HOT_GLYPH_BUF_SIZE={HOT_GLYPH_BUF_SIZE}; raise it in lib/EpdFont/FontDecompressor.h "
                  f"(costs FALLBACK_CACHE_SLOTS x the increase in .bss) and update this constant",
                  file=sys.stderr)
            sys.exit(1)

        all_glyphs.append((glyph, packed))

print(f"ink re-centred: {recentred_glyphs} of {len(all_glyphs)} glyphs moved back by a pixel", file=sys.stderr)

# pipe seems to be a good heuristic for the "real" descender
face = load_glyph(ord('|'))

glyph_data = []
glyph_props = []
for index, glyph in enumerate(all_glyphs):
    props, packed = glyph
    glyph_data.extend([b for b in packed])
    glyph_props.append(props)

# --- Kerning pair extraction ---
# Modern fonts store kerning in the OpenType GPOS table, which FreeType's
# get_kerning() does not read. kern_extract.py parses both the legacy
# kern table and the GPOS 'kern' feature with fonttools, for this script and
# fontconvert_sdcard.py alike.

COMBINING_MARKS_START = 0x0300
COMBINING_MARKS_END = 0x036F
all_codepoints = [g.code_point for g in glyph_props]
kernable_codepoints = set(cp for cp in all_codepoints
                          if not (COMBINING_MARKS_START <= cp <= COMBINING_MARKS_END))

# Map each kernable codepoint to the font-stack index that serves it
# (same priority logic as load_glyph).
cp_to_face_idx = {}
for cp in kernable_codepoints:
    for face_idx, f in enumerate(font_stack):
        if f.get_char_index(cp) > 0:
            cp_to_face_idx[cp] = face_idx
            break

# Group codepoints by face index
face_idx_cps = {}
for cp, fi in cp_to_face_idx.items():
    face_idx_cps.setdefault(fi, set()).add(cp)

def extract_kerning_fonttools(font_path, codepoints):
    """Extract kerning pairs from a font file using fonttools.

    Returns ({(leftCp, rightCp): design_units}, units_per_em) for the given
    codepoints, non-zero values only. Nothing is scaled here: see below.
    """
    font = TTFont(font_path)
    units_per_em = font['head'].unitsPerEm
    cmap = font.getBestCmap() or {}

    # Build glyph_name -> codepoint map (only for requested codepoints)
    glyph_to_cp = {}
    for cp in codepoints:
        gname = cmap.get(cp)
        if gname:
            glyph_to_cp[gname] = cp

    # Raw kerning values in font design units
    raw_kern = extract_raw_kerning(font, glyph_to_cp)  # (left_glyph_name, right_glyph_name) -> design_units

    font.close()

    result = {}  # (leftCp, rightCp) -> design units
    for (lg, rg), du in raw_kern.items():
        if du != 0:
            result[(glyph_to_cp[lg], glyph_to_cp[rg])] = du
    return result, units_per_em

# The ppem used by the existing glyph rasterization:
#   face.set_char_size(size << 6, size << 6, 150, 150)
# means size_pt at 150 DPI -> ppem = size * 150 / 72
ppem = size * 150.0 / 72.0

# The kerning STRUCTURE -- which code points kern, their classes, which class pairs have a value --
# is worked out in design units, and only the values are scaled to this size. Every size of a
# typeface then emits identical codepoint, class and sparse-column tables, which
# dedupe_font_tables.py keeps once for the whole family. Built from values already scaled, the
# structure followed the rounding: at a small size two pairs could round alike (or to 0) where a
# larger size kept them apart, and that size lost the shared tables (inter_ui_10_bold: ~3.3 KB).
# Design units from a stack of fonts are made comparable by scaling each to the stack's common
# units-per-em multiple, which keeps them exact integers.
_kern_by_face = []
for face_idx, cps in face_idx_cps.items():
    _kern_by_face.append(extract_kerning_fonttools(args.fontstack[face_idx], cps))
kern_units_per_em = math.lcm(*(upem for _, upem in _kern_by_face)) if _kern_by_face else 1
kern_map = {}  # (leftCp, rightCp) -> design units at kern_units_per_em
for face_kern, upem in _kern_by_face:
    kern_map.update({pair: du * (kern_units_per_em // upem) for pair, du in face_kern.items()})

print(f"kerning: {len(kern_map)} pairs extracted", file=sys.stderr)

# A class number is one byte (uint8_t in EpdFontData): past 255 classes a side, merge the closest
# (kern_reduce.py). Without this the tables were written anyway, with class numbers that do not fit.
_pairs = len(kern_map)
kern_map, _reduced = reduce_kern_classes(kern_map)
if _reduced:
    print(f"WARNING: kerning needed {_reduced['left'][0]} left and {_reduced['right'][0]} right classes, "
          f"more than the 255 a font can store; merged the closest to {_reduced['left'][1]} and "
          f"{_reduced['right'][1]} ({_reduced['pairs_changed']} of {_pairs} pairs changed)",
          file=sys.stderr)

# --- Derive class-based kerning from pairs ---
kern_left_classes = []   # list of (codepoint, classId)
kern_right_classes = []  # list of (codepoint, classId)
kern_matrix = []         # flat list of int8_t values
kern_left_class_count = 0
kern_right_class_count = 0

if kern_map:
    all_left_cps = {lcp for lcp, _ in kern_map}
    all_right_cps = {rcp for _, rcp in kern_map}

    sorted_right_cps = sorted(all_right_cps)
    sorted_left_cps = sorted(all_left_cps)

    # Group left codepoints by identical adjustment row
    left_profile_to_class = {}
    left_class_map = {}
    left_class_id = 1
    for lcp in sorted(all_left_cps):
        row = tuple(kern_map.get((lcp, rcp), 0) for rcp in sorted_right_cps)
        if row not in left_profile_to_class:
            left_profile_to_class[row] = left_class_id
            left_class_id += 1
        left_class_map[lcp] = left_profile_to_class[row]

    # Group right codepoints by identical adjustment column
    right_profile_to_class = {}
    right_class_map = {}
    right_class_id = 1
    for rcp in sorted(all_right_cps):
        col = tuple(kern_map.get((lcp, rcp), 0) for lcp in sorted_left_cps)
        if col not in right_profile_to_class:
            right_profile_to_class[col] = right_class_id
            right_class_id += 1
        right_class_map[rcp] = right_profile_to_class[col]

    kern_left_class_count = left_class_id - 1
    kern_right_class_count = right_class_id - 1

    # Build the class x class matrix: which cells have a value (design units, the same at every
    # size) and the value at this size, scaled to 4.4 fixed-point pixels. A cell can round to 0
    # here and still be written, so its place in the shared sparse columns stays the same.
    kern_present = [False] * (kern_left_class_count * kern_right_class_count)
    kern_matrix = [0] * (kern_left_class_count * kern_right_class_count)
    kern_scale = ppem / kern_units_per_em
    for (lcp, rcp), du in kern_map.items():
        lc = left_class_map[lcp] - 1
        rc = right_class_map[rcp] - 1
        kern_present[lc * kern_right_class_count + rc] = True
        kern_matrix[lc * kern_right_class_count + rc] = fp4_from_design_units(du, kern_scale)

    # Build sorted class entry lists
    kern_left_classes = sorted(left_class_map.items())
    kern_right_classes = sorted(right_class_map.items())

    matrix_size = kern_left_class_count * kern_right_class_count
    entries_size = (len(kern_left_classes) + len(kern_right_classes)) * 3
    print(f"kerning: {kern_left_class_count} left classes, {kern_right_class_count} right classes, "
          f"{matrix_size + entries_size} bytes", file=sys.stderr)

# --- Ligature pair extraction ---
# Parse the OpenType GSUB table for LigatureSubst (type 4) lookups.
# Multi-character ligatures (3+ codepoints) are decomposed into chained
# pairs when an intermediate ligature exists (e.g., ffi = ff + i where ff
# is itself a ligature). Only pairs where both input codepoints and the
# output codepoint are in the generated glyph set are included.

all_codepoints_set = set(all_codepoints)

# Standard Unicode ligature codepoints for known input sequences.
# Used as a fallback when the GSUB substitute glyph has no cmap entry.
STANDARD_LIGATURE_MAP = {
    (0x66, 0x66):       0xFB00,  # ff
    (0x66, 0x69):       0xFB01,  # fi
    (0x66, 0x6C):       0xFB02,  # fl
    (0x66, 0x66, 0x69): 0xFB03,  # ffi
    (0x66, 0x66, 0x6C): 0xFB04,  # ffl
    (0x17F, 0x74):      0xFB05,  # long-s + t
    (0x73, 0x74):       0xFB06,  # st
}

def extract_ligatures_fonttools(font_path, codepoints):
    """Extract ligature substitution pairs from a font file using fonttools.

    Returns list of (packed_pair, ligature_codepoint) for the given codepoints.
    Multi-character ligatures are decomposed into chained pairs.
    """
    font = TTFont(font_path)
    cmap = font.getBestCmap() or {}

    # Build glyph_name -> codepoint and codepoint -> glyph_name maps
    glyph_to_cp = {}
    cp_to_glyph = {}
    for cp, gname in cmap.items():
        glyph_to_cp[gname] = cp
        cp_to_glyph[cp] = gname

    # Collect raw ligature rules: (sequence_of_codepoints) -> ligature_codepoint
    raw_ligatures = {}  # tuple of codepoints -> ligature codepoint

    if 'GSUB' in font:
        gsub = font['GSUB'].table

        # Find lookup indices for ligature features.
        # Currently extracts 'liga' (standard) and 'rlig' (required) only.
        # To also extract discretionary or historical ligatures, add:
        #   'dlig' - Discretionary Ligatures (e.g., ft, st in Bookerly)
        #   'hlig' - Historical Ligatures (e.g., long-s+t in OpenDyslexic)
        # These are off by default in standard text renderers.
        LIGATURE_FEATURES = ('liga', 'rlig')
        liga_lookup_indices = set()
        if gsub.FeatureList:
            for fr in gsub.FeatureList.FeatureRecord:
                if fr.FeatureTag in LIGATURE_FEATURES:
                    liga_lookup_indices.update(fr.Feature.LookupListIndex)

        for li in liga_lookup_indices:
            lookup = gsub.LookupList.Lookup[li]
            for st in lookup.SubTable:
                actual = st
                # Unwrap Extension (lookup type 7) wrappers
                if lookup.LookupType == 7 and hasattr(st, 'ExtSubTable'):
                    actual = st.ExtSubTable
                # LigatureSubst is lookup type 4
                if not hasattr(actual, 'ligatures'):
                    continue
                for first_glyph, ligature_list in actual.ligatures.items():
                    if first_glyph not in glyph_to_cp:
                        continue
                    first_cp = glyph_to_cp[first_glyph]
                    for lig in ligature_list:
                        # lig.Component is a list of subsequent glyph names
                        # lig.LigGlyph is the substitute glyph name
                        component_cps = []
                        valid = True
                        for comp_glyph in lig.Component:
                            if comp_glyph not in glyph_to_cp:
                                valid = False
                                break
                            component_cps.append(glyph_to_cp[comp_glyph])
                        if not valid:
                            continue
                        seq = tuple([first_cp] + component_cps)
                        if lig.LigGlyph in glyph_to_cp:
                            lig_cp = glyph_to_cp[lig.LigGlyph]
                        elif seq in STANDARD_LIGATURE_MAP:
                            lig_cp = STANDARD_LIGATURE_MAP[seq]
                        else:
                            seq_str = ', '.join(f'U+{cp:04X}' for cp in seq)
                            print(f"ligatures: WARNING: dropping ligature ({seq_str}) -> "
                                  f"glyph '{lig.LigGlyph}': output glyph has no cmap entry "
                                  f"and input sequence is not in STANDARD_LIGATURE_MAP",
                                  file=sys.stderr)
                            continue
                        raw_ligatures[seq] = lig_cp

    font.close()

    # Filter: only keep ligatures where all input and output codepoints are
    # in our generated glyph set
    filtered = {}
    for seq, lig_cp in raw_ligatures.items():
        if lig_cp not in codepoints and lig_cp not in all_codepoints_set:
            continue
        if all(cp in codepoints for cp in seq):
            filtered[seq] = lig_cp

    # Decompose into chained pairs
    # For 2-codepoint sequences: direct pair (a, b) -> lig
    # For 3+ codepoint sequences: chain through intermediates
    #   e.g., (f, f, i) -> ffi requires (f, f) -> ff to exist,
    #   then we add (ff, i) -> ffi
    pairs = []
    # First pass: collect all 2-codepoint ligatures
    two_char = {seq: lig_cp for seq, lig_cp in filtered.items() if len(seq) == 2}
    for seq, lig_cp in two_char.items():
        packed = (seq[0] << 16) | seq[1]
        pairs.append((packed, lig_cp))

    # Second pass: decompose 3+ codepoint ligatures into chained pairs
    for seq, lig_cp in filtered.items():
        if len(seq) < 3:
            continue
        # Try to find an intermediate: check if the first N-1 codepoints
        # form a known ligature, then chain (intermediate, last) -> lig
        prefix = seq[:-1]
        last_cp = seq[-1]
        if prefix in filtered:
            intermediate_cp = filtered[prefix]
            packed = (intermediate_cp << 16) | last_cp
            pairs.append((packed, lig_cp))
        else:
            print(f"ligatures: skipping {len(seq)}-char ligature "
                  f"({', '.join(f'U+{cp:04X}' for cp in seq)}) -> U+{lig_cp:04X}: "
                  f"no intermediate ligature for prefix", file=sys.stderr)

    return pairs

ligature_codepoints = set(cp for cp in all_codepoints
                          if not (COMBINING_MARKS_START <= cp <= COMBINING_MARKS_END))

# Map ligature codepoints to the font-stack index that serves them
lig_cp_to_face_idx = {}
for cp in ligature_codepoints:
    for face_idx, f in enumerate(font_stack):
        if f.get_char_index(cp) > 0:
            lig_cp_to_face_idx[cp] = face_idx
            break

# Group by face index
lig_face_idx_cps = {}
for cp, fi in lig_cp_to_face_idx.items():
    lig_face_idx_cps.setdefault(fi, set()).add(cp)

ligature_pairs = []
for face_idx, cps in lig_face_idx_cps.items():
    font_path = args.fontstack[face_idx]
    ligature_pairs.extend(extract_ligatures_fonttools(font_path, cps))

# Deduplicate (keep first occurrence) and sort
seen_lig_keys = set()
unique_ligature_pairs = []
for packed, lig_cp in ligature_pairs:
    if packed not in seen_lig_keys:
        seen_lig_keys.add(packed)
        unique_ligature_pairs.append((packed, lig_cp))
ligature_pairs = sorted(unique_ligature_pairs, key=lambda p: p[0])
print(f"ligatures: {len(ligature_pairs)} pairs extracted", file=sys.stderr)

compress = args.compress

if args.zopfli and not compress:
    print("Error: --zopfli requires --compress", file=sys.stderr)
    sys.exit(1)


def deflate_raw(data, wbits=15, force_zlib=False):
    """Raw-DEFLATE compress `data` (no zlib/gzip wrapper), decodable on-device by
    uzlib via inflate(wbits=-15). Uses Zopfli when --zopfli is set (a few percent
    smaller than zlib -9, much slower — fine at font-generation time), else zlib -9.
    Zopfli's Python binding only emits zlib-wrapped output, so strip the 2-byte
    header and 4-byte adler32 trailer to recover the raw DEFLATE block. A round-trip
    assert guards against any wrapper-format surprise.

    `wbits` bounds how far back the encoder may reference, which is what bounds the
    decoder's ring (see measure_ring). Zopfli has no such knob — its window is a
    compile-time constant — so force_zlib selects the bounded encoder instead."""
    if args.zopfli and not force_zlib:
        import zopfli.zlib
        wrapped = zopfli.zlib.compress(bytes(data))
        raw = wrapped[2:-4]  # drop zlib CMF/FLG header + adler32 trailer
    else:
        compressor = zlib.compressobj(level=9, wbits=-wbits)
        raw = compressor.compress(bytes(data)) + compressor.flush()
    assert zlib.decompress(raw, -15) == bytes(data), "raw-DEFLATE round-trip failed"
    return raw


# --- Back-reference distance measurement -------------------------------------------------
#
# EpdFontGroup::ringBytes is the RAM the on-device decoder needs to stream a group, and it is
# the largest back-reference the finished stream actually contains — not the group size and not
# 2^wbits, both of which overstate it. So measure the real streams.
#
# It has to be read out of the bitstream. Probing by decompressing at a small -wbits does NOT
# work: zlib-ng (which ships with Python 3.14) does not enforce the window on raw inflate and
# happily decodes a stream with a verified 24 KB reference at wbits=9.

_LEN_EXTRA = [0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0]
_DIST_BASE = [1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
              1025,1537,2049,3073,4097,6145,8193,12289,16385,24577]
_DIST_EXTRA = [0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13]
_CLEN_ORDER = [16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15]


class _BitReader:
    def __init__(self, data):
        self.d, self.pos = data, 0

    def bit(self):
        b = (self.d[self.pos >> 3] >> (self.pos & 7)) & 1
        self.pos += 1
        return b

    def bits(self, n):
        v = 0
        for i in range(n):
            v |= self.bit() << i
        return v


def _huffman(lengths):
    """Canonical Huffman code lengths -> {(bit_length, code): symbol}."""
    maxlen = max(lengths) if lengths else 0
    bl_count = [0] * (maxlen + 1)
    for l in lengths:
        if l:
            bl_count[l] += 1
    code, next_code = 0, [0] * (maxlen + 2)
    for b in range(1, maxlen + 1):
        code = (code + bl_count[b - 1]) << 1
        next_code[b] = code
    table = {}
    for sym, l in enumerate(lengths):
        if l:
            table[(l, next_code[l])] = sym
            next_code[l] += 1
    return table


def _decode_sym(br, table):
    code, length = 0, 0
    while True:
        code = (code << 1) | br.bit()
        length += 1
        if (length, code) in table:
            return table[(length, code)]
        if length > 15:
            raise ValueError("corrupt Huffman code while measuring ring size")


_FIXED_LIT = _huffman([8] * 144 + [9] * 112 + [7] * 24 + [8] * 8)
_FIXED_DIST = _huffman([5] * 30)


def measure_ring(raw):
    """Largest back-reference distance in a raw-DEFLATE stream, i.e. the smallest ring that
    decodes it. 0 when the stream is pure literals."""
    br = _BitReader(raw)
    best = 0
    while True:
        final = br.bit()
        btype = br.bits(2)
        if btype == 0:                      # stored
            br.pos = (br.pos + 7) & ~7
            n = br.bits(16)
            br.bits(16)
            br.pos += n * 8
        else:
            if btype == 1:
                lit, dist = _FIXED_LIT, _FIXED_DIST
            elif btype == 2:
                hlit, hdist, hclen = br.bits(5) + 257, br.bits(5) + 1, br.bits(4) + 4
                cl = [0] * 19
                for i in range(hclen):
                    cl[_CLEN_ORDER[i]] = br.bits(3)
                cl_tab = _huffman(cl)
                lengths = []
                while len(lengths) < hlit + hdist:
                    s = _decode_sym(br, cl_tab)
                    if s < 16:
                        lengths.append(s)
                    elif s == 16:
                        lengths += [lengths[-1]] * (br.bits(2) + 3)
                    elif s == 17:
                        lengths += [0] * (br.bits(3) + 3)
                    else:
                        lengths += [0] * (br.bits(7) + 11)
                lit = _huffman(lengths[:hlit])
                dist = _huffman(lengths[hlit:hlit + hdist])
            else:
                raise ValueError("reserved DEFLATE block type")
            while True:
                sym = _decode_sym(br, lit)
                if sym == 256:
                    break
                if sym < 256:
                    continue
                br.bits(_LEN_EXTRA[sym - 257])
                ds = _decode_sym(br, dist)
                best = max(best, _DIST_BASE[ds] + br.bits(_DIST_EXTRA[ds]))
        if final:
            return best


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
if compress:
    # Script-based grouping: glyphs that co-occur in typical text rendering
    # are grouped together for efficient LRU caching on the embedded target.
    # Since glyphs are in codepoint order, glyphs in the same Unicode block
    # are contiguous in the array and form natural groups.
    #
    # A hard size cap (GROUP_MAX_UNCOMPRESSED_BYTES) is applied on top of script
    # boundaries: if adding the next glyph would push the uncompressed group size
    # over the cap, the group is closed and a new one started with the same script
    # ID. This keeps the embedded decompressor's transient malloc bounded regardless
    # of font density (CJK, Vietnamese, user-supplied fonts with large Unicode blocks).
    SCRIPT_GROUP_RANGES = [
        (0x0000, 0x007F),   # ASCII
        (0x0080, 0x00FF),   # Latin-1 Supplement
        (0x0100, 0x017F),   # Latin Extended-A
        (0x0180, 0x024F),   # Latin Extended-B
        # IPA Extensions + Spacing Modifier Letters, for dictionary pronunciation.
        # These already grouped correctly without being listed -- an unlisted
        # codepoint gets script id -1, and a contiguous run of them becomes one
        # group like any other. Listed anyway so the grouping is deliberate
        # rather than a side effect of the ranges above stopping at 0x024F, and
        # so inserting a future range between them cannot silently split it.
        # Verified byte-identical output before and after adding this line.
        (0x0250, 0x02FF),
        (0x0300, 0x036F),   # Combining Diacritical Marks
        (0x0400, 0x04FF),   # Cyrillic
        (0x1EA0, 0x1EF9),   # Vietnamese Extended
        (0x2000, 0x206F),   # General Punctuation
        (0x2070, 0x209F),   # Superscripts & Subscripts
        (0x20A0, 0x20CF),   # Currency Symbols
        (0x2190, 0x21FF),   # Arrows
        (0x2200, 0x22FF),   # Math Operators
        (0xFB00, 0xFB06),   # Alphabetic Presentation Forms (ligatures)
        (0xFFFD, 0xFFFD),   # Replacement Character
    ]

    # The decoder streams a group through a ring of EpdFontGroup::ringBytes and compacts each
    # glyph out of the passing bytes (FontDecompressor::GroupStream), so its peak transient is
    # the largest back-reference the stream contains -- NOT the group's uncompressed size. That
    # decoupling is what these two constants express, and they now do different jobs:
    #
    #   GROUP_RING_MAX_BYTES        bounds RAM.  Enforced per group by picking an encoding whose
    #                               measured distances fit, and by splitting the group when no
    #                               encoding does.
    #   GROUP_MAX_UNCOMPRESSED_BYTES bounds CPU. A glyph is reached by decoding forward from the
    #                               group start, so this caps the worst-case decode to reach one
    #                               glyph. It no longer has anything to do with RAM.
    #
    # Sizing the ring, measured over all 40 compressed built-in fonts (2026-08-19):
    #   ring    compressed total   vs shipped
    #   32 KB   1264010            -7.9%       (= the old uncapped 64 KB-group build)
    #    8 KB   1350611            -1.6%
    #    4 KB   1404302            +2.3%
    #    2 KB   1442752            +5.1%
    # Against the mechanism this replaces -- capping the GROUP -- the ring is strictly the
    # better dial: capping charged +2.3% (1403850) for a 6 KB transient, where the ring buys a
    # 4 KB transient for the same flash, and at an equal 8 KB transient it hands 22 KB back.
    #
    # 4 KB halves the peak transient against the 8 KB group cap it replaces. Mid-build draws
    # have been measured with contig as low as 11252, so the margin roughly triples.
    #
    # The question this answers was raised by crosspoint-reader PR #3083 ("compress built-in
    # fonts with per-glyph GlyphStream codec", Sung-jin Brian Hong / @serialx), which attacks the
    # same transient-allocation problem by giving every glyph its own independently decodable
    # stream. None of that code is used here and the approach is deliberately different: measured
    # on this corpus, per-glyph DEFLATE costs +99.9% flash with no dictionary and +41.0% with the
    # best shared dictionary (8 KB). That cost is exactly why #3083 needed a trained range coder,
    # and it paid for that with a 2x page render on its author's own device measurement. Bounding
    # the window keeps the codec we already ship and needs no per-glyph framing at all.
    GROUP_RING_MAX_BYTES = 4096
    GROUP_MAX_UNCOMPRESSED_BYTES = 65536

    # zlib refuses raw windows below 2^9, and a group split to GROUP_RING_MAX_BYTES can never
    # reference further back than its own length, so a fitting encoding always exists.
    assert GROUP_RING_MAX_BYTES >= 512, "no encoder can honour a ring below 512 bytes"

    def get_script_group(code_point):
        for i, (start, end) in enumerate(SCRIPT_GROUP_RANGES):
            if start <= code_point <= end:
                return i
        return -1

    # Byte-aligned payload per glyph, kept so a run can be re-split at glyph boundaries below.
    aligned_of = [
        to_byte_aligned(packed, props.width, props.height) if props.width > 0 and props.height > 0 else b''
        for props, packed in all_glyphs
    ]

    # Split on script boundaries, then on the CPU cap.
    runs = []  # list of (first_glyph_index, glyph_count)
    current_group_id = None
    group_start = 0
    group_count = 0
    group_uncompressed = 0

    for i, (props, _) in enumerate(all_glyphs):
        sg = get_script_group(props.code_point)
        glyph_aligned_size = len(aligned_of[i])
        if glyph_aligned_size > GROUP_MAX_UNCOMPRESSED_BYTES:
            raise ValueError(
                f"Glyph {i} (code point U+{props.code_point:04X}) single aligned size "
                f"{glyph_aligned_size} exceeds GROUP_MAX_UNCOMPRESSED_BYTES={GROUP_MAX_UNCOMPRESSED_BYTES}"
            )
        size_overflow = group_uncompressed + glyph_aligned_size > GROUP_MAX_UNCOMPRESSED_BYTES

        if sg != current_group_id or size_overflow:
            if group_count > 0:
                runs.append((group_start, group_count))
            current_group_id = sg
            group_start = i
            group_count = 1
            group_uncompressed = glyph_aligned_size
        else:
            group_count += 1
            group_uncompressed += glyph_aligned_size

    if group_count > 0:
        runs.append((group_start, group_count))

    def encode_block(blob):
        """Smallest raw-DEFLATE encoding of `blob` whose back-references fit the ring ceiling,
        as (raw, ring). Zopfli first — it wins outright whenever its distances happen to fit —
        then progressively tighter bounded-window zlib. wbits=10 caps distances at 1024, so a
        fitting candidate always exists."""
        best = None
        candidates = ([(15, False)] if args.zopfli else []) + [(w, True) for w in (15, 14, 13, 12, 11, 10)]
        for wbits, force_zlib in candidates:
            raw = deflate_raw(blob, wbits=wbits, force_zlib=force_zlib)
            ring = min(measure_ring(raw), len(blob))
            if ring > GROUP_RING_MAX_BYTES:
                continue
            if best is None or len(raw) < len(best[0]):
                best = (raw, ring)
        assert best is not None, "no encoding fits the ring ceiling (should be unreachable)"
        return best

    def encode_run(first_idx, count):
        """One script run -> [(first_idx, count, raw, ring)].

        Kept whole when a fitting encoding is small enough, else split into ring-sized pieces,
        which lets each piece use unbounded zopfli again (a stream cannot reference further back
        than its own start). Whichever is smaller wins; both honour the ring ceiling."""
        whole_raw, whole_ring = encode_block(b''.join(aligned_of[first_idx:first_idx + count]))

        pieces = []  # (first_idx, count, blob)
        piece_start, piece = first_idx, bytearray()
        for gi in range(first_idx, first_idx + count):
            if piece and len(piece) + len(aligned_of[gi]) > GROUP_RING_MAX_BYTES:
                pieces.append((piece_start, gi - piece_start, bytes(piece)))
                piece_start, piece = gi, bytearray()
            piece.extend(aligned_of[gi])
        pieces.append((piece_start, first_idx + count - piece_start, bytes(piece)))

        if len(pieces) == 1:
            return [(first_idx, count, whole_raw, whole_ring)]
        split = [(s, c) + encode_block(b) for s, c, b in pieces]
        if sum(len(r) for _, _, r, _ in split) < len(whole_raw):
            return split
        return [(first_idx, count, whole_raw, whole_ring)]

    groups = [g for first_idx, count in runs for g in encode_run(first_idx, count)]

    # Emit: assign each glyph its within-group packed offset and concatenate the streams.
    compressed_groups = []  # (compressed_bytes, uncompressed_size, glyph_count, first_glyph_index, ring)
    compressed_bitmap_data = []
    modified_glyph_props = list(glyph_props)

    for first_idx, count, compressed, ring in groups:
        packed_len = 0
        uncompressed_size = 0
        for gi in range(first_idx, first_idx + count):
            _, packed = all_glyphs[gi]
            old_props = modified_glyph_props[gi]
            modified_glyph_props[gi] = GlyphProps(
                width=old_props.width,
                height=old_props.height,
                advance_x=old_props.advance_x,
                left=old_props.left,
                top=old_props.top,
                data_length=old_props.data_length,
                data_offset=packed_len,  # within-group packed offset
                code_point=old_props.code_point,
            )
            packed_len += len(packed)
            uncompressed_size += len(aligned_of[gi])

        assert ring <= GROUP_RING_MAX_BYTES, f"group at {first_idx} needs a {ring}-byte ring"
        compressed_groups.append((compressed, uncompressed_size, count, first_idx, ring))
        compressed_bitmap_data.extend(compressed)

    glyph_props = modified_glyph_props
    total_compressed = len(compressed_bitmap_data)
    total_uncompressed = len(glyph_data)
    peak_ring = max(r for _, _, _, _, r in compressed_groups)
    peak_group = max(u for _, u, _, _, _ in compressed_groups)
    print(f"// Compression: {total_uncompressed} -> {total_compressed} bytes "
          f"({100*total_compressed/total_uncompressed:.1f}%), {len(groups)} groups, "
          f"peak ring {peak_ring} B (group {peak_group} B)", file=sys.stderr)

print(f"""/**
 * generated by fontconvert.py
 * name: {font_name}
 * size: {size}
 * mode: {'2-bit' if is2Bit else '1-bit'}{('  compressed: ' + ('zopfli' if args.zopfli else 'zlib')) if compress else ''}
 * hinting: auto (FT_LOAD_FORCE_AUTOHINT — grid-fits stems to whole pixels; ink re-centred on the unhinted outline)
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

def cp_label(cp):
    if cp == 0x5C:
        return '<backslash>'
    return chr(cp) if 0x20 < cp < 0x7F else f'U+{cp:04X}'

# EpdGlyphPacked: 6 bytes instead of EpdGlyph's 16. dataLength is derived on device from
# width * height * bpp, left/top are narrowed to int8, and dataOffset is not stored at all. Each
# of those is CHECKED here rather than assumed, because every one of them fails silently at
# runtime -- a wrong length reads a neighbouring glyph's bitmap, which renders as the wrong
# character rather than as a crash. See EpdGlyphPacked in EpdFontData.h.
bpp = 2 if is2Bit else 1
for g in glyph_props:
    derived = (g.width * g.height * bpp + 7) // 8
    if derived != g.data_length:
        sys.exit(f"{font_name}: U+{g.code_point:04X} dataLength {g.data_length} != derived {derived}. "
                 f"The bitmap packing is no longer a continuous bit stream, so EpdGlyphPacked cannot "
                 f"derive it -- restore the stored field or fix the packer.")
    if not (-128 <= g.left <= 127):
        sys.exit(f"{font_name}: U+{g.code_point:04X} left {g.left} does not fit int8_t")
    if not (-128 <= g.top <= 127):
        sys.exit(f"{font_name}: U+{g.code_point:04X} top {g.top} does not fit int8_t")
    if not (0 <= g.advance_x <= 0xFFFF):
        sys.exit(f"{font_name}: U+{g.code_point:04X} advanceX {g.advance_x} exceeds uint16")

# Only an UNCOMPRESSED font needs its bitmap offsets: it indexes a flat array directly. A
# compressed font's bitmap comes out of a decoded group, and FontDecompressor::getAlignedOffset()
# finds the position by walking the group itself -- it never read the stored offset, which is
# what made the field removable rather than merely shrinkable.
if not compress:
    for g in glyph_props:
        if not (0 <= g.data_offset <= 0xFFFF):
            sys.exit(f"{font_name}: U+{g.code_point:04X} dataOffset {g.data_offset} exceeds uint16. "
                     f"An uncompressed font is capped at 64 KB of bitmap data by the uint16 entries "
                     f"of EpdFontData::bitmapOffsets; either compress this face or widen that table "
                     f"to uint32.")

print(f"static const EpdGlyphPacked {font_name}Glyphs[] = {{")
for g in glyph_props:
    print(f"    {{ {g.width}, {g.height}, {g.advance_x}, {g.left}, {g.top} }},"
          f" // {cp_label(g.code_point)}")
print ("};\n");

# Uncompressed faces only -- see the note above the bound check.
if not compress:
    print(f"static const uint16_t {font_name}BitmapOffsets[] = {{")
    for g in glyph_props:
        print(f"    {g.data_offset}, // {cp_label(g.code_point)}")
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
    for compressed, uncompressed_size, count, first_idx, ring in compressed_groups:
        print(f"    {{ {compressed_offset}, {len(compressed)}, {uncompressed_size}, {count}, {ring}, {first_idx} }},")
        compressed_offset += len(compressed)
    print("};\n")

if kern_map:
    # Split class maps: codepoints in one array, class IDs in a parallel one. Same 3 bytes per
    # entry as the packed EpdKernClassEntry, but the binary search only reads codepoints, so
    # keeping the payload out of the searched array shrinks its footprint by a third and makes
    # every read naturally aligned. Measured -13 to -14% on the class lookup, which is ~96% of
    # getKerning(). SD-card fonts keep the packed form (fontconvert_sdcard.py) because .cpfont
    # stores it verbatim and maps it in place.
    for side, entries in (("Left", kern_left_classes), ("Right", kern_right_classes)):
        print(f"static const uint16_t {font_name}Kern{side}Codepoints[] = {{")
        for chunk in chunks([cp for cp, _ in entries], 12):
            print("    " + ", ".join(f"0x{cp:04X}" for cp in chunk) + ",")
        print("};\n")
        print(f"static const uint8_t {font_name}Kern{side}ClassIds[] = {{")
        for chunk in chunks([cls for _, cls in entries], 16):
            print("    " + ", ".join(f"{cls:3d}" for cls in chunk) + ",")
        print("};\n")

    # Sparse (CSR) kerning. The dense leftClass x rightClass matrix is overwhelmingly zero —
    # measured 86.6% across the built-in set — and at 40 fonts the dense form cost ~583 KB of
    # flash against ~165 KB for this one. Values are unchanged, so nothing repaginates.
    # SD-card fonts still emit the dense matrix (fontconvert_sdcard.py): the .cpfont format is
    # mapped in place and changing it would break font files already on users' cards.
    row_offsets = []
    sparse_cols = []
    sparse_vals = []
    for row in range(kern_left_class_count):
        row_offsets.append(len(sparse_cols))
        row_start = row * kern_right_class_count
        for col in range(kern_right_class_count):
            if kern_present[row_start + col]:
                sparse_cols.append(col)
                sparse_vals.append(kern_matrix[row_start + col])
    row_offsets.append(len(sparse_cols))
    if len(sparse_cols) > 0xFFFF:
        print(f"Error: {len(sparse_cols)} kern entries exceed the uint16 row-offset range", file=sys.stderr)
        sys.exit(1)
    if kern_right_class_count > 256:
        print(f"Error: {kern_right_class_count} right classes exceed the uint8 column range", file=sys.stderr)
        sys.exit(1)
    dense_bytes = kern_left_class_count * kern_right_class_count
    sparse_bytes = len(row_offsets) * 2 + len(sparse_cols) * 2
    print(f"// Kerning: {len(sparse_cols)} of {dense_bytes} entries non-zero "
          f"({100.0 * len(sparse_cols) / dense_bytes:.1f}%), {dense_bytes} -> {sparse_bytes} bytes",
          file=sys.stderr)

    print(f"static const uint16_t {font_name}KernRowOffsets[] = {{")
    for chunk in chunks(row_offsets, 16):
        print("    " + ", ".join(f"{v:5d}" for v in chunk) + ",")
    print("};\n")

    print(f"static const uint8_t {font_name}KernSparseCols[] = {{")
    for chunk in chunks(sparse_cols, 16):
        print("    " + ", ".join(f"{v:3d}" for v in chunk) + ",")
    print("};\n")

    print(f"static const int8_t {font_name}KernSparseValues[] = {{")
    for chunk in chunks(sparse_vals, 16):
        print("    " + ", ".join(f"{v:4d}" for v in chunk) + ",")
    print("};\n")

if ligature_pairs:
    print(f"static const EpdLigaturePair {font_name}LigaturePairs[] = {{")
    for packed_pair, lig_cp in ligature_pairs:
        print(f"    {{ 0x{packed_pair:08X}, 0x{lig_cp:04X} }}, // {cp_label(packed_pair >> 16)} {cp_label(packed_pair & 0xFFFF)} -> {cp_label(lig_cp)}")
    print("};\n")

print(f"static const EpdFontData {font_name} = {{")
print(f"    {font_name}Bitmaps,")
print("    nullptr,  // glyph: the 16-byte EpdGlyph array is the .cpfont form, SD-card fonts only")
print(f"    {font_name}Glyphs,")
if compress:
    print("    nullptr,  // bitmapOffsets: a compressed font recovers the offset from the group walk")
else:
    print(f"    {font_name}BitmapOffsets,")
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
# glyphToGroup (not used for script-grouped fonts)
print("    nullptr,")
if kern_map:
    print("    nullptr,  // kernLeftClasses: built-in fonts use the split arrays below")
    print("    nullptr,  // kernRightClasses")
    print(f"    {font_name}KernLeftCodepoints,")
    print(f"    {font_name}KernLeftClassIds,")
    print(f"    {font_name}KernRightCodepoints,")
    print(f"    {font_name}KernRightClassIds,")
    print("    nullptr,  // kernMatrix: built-in fonts use the sparse form below")
    print(f"    {font_name}KernRowOffsets,")
    print(f"    {font_name}KernSparseCols,")
    print(f"    {font_name}KernSparseValues,")
    print(f"    {len(kern_left_classes)},")
    print(f"    {len(kern_right_classes)},")
    print(f"    {kern_left_class_count},")
    print(f"    {kern_right_class_count},")
else:
    for _ in range(10):
        print(f"    nullptr,")
    print(f"    0,")
    print(f"    0,")
    print(f"    0,")
    print(f"    0,")
if ligature_pairs:
    print(f"    {font_name}LigaturePairs,")
    print(f"    {len(ligature_pairs)},")
else:
    print(f"    nullptr,")
    print(f"    0,")
print("};")
