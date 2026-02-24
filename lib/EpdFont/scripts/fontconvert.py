#!python3
import freetype
import zlib
import sys
import re
import math
import argparse
from collections import namedtuple
from fontTools.ttLib import TTFont

# Originally from https://github.com/vroland/epdiy

parser = argparse.ArgumentParser(description="Generate a header file from a font to be used with epdiy.")
parser.add_argument("name", action="store", help="name of the font.")
parser.add_argument("size", type=int, help="font size to use.")
parser.add_argument("fontstack", action="store", nargs='+', help="list of font files, ordered by descending priority.")
parser.add_argument("--2bit", dest="is2Bit", action="store_true", help="generate 2-bit greyscale bitmap instead of 1-bit black and white.")
parser.add_argument("--additional-intervals", dest="additional_intervals", action="append", help="Additional code point intervals to export as min,max. This argument can be repeated.")
parser.add_argument("--compress", dest="compress", action="store_true", help="Compress glyph bitmaps using DEFLATE with group-based compression.")
parser.add_argument("--force-autohint", dest="force_autohint", action="store_true", help="Force FreeType auto-hinter instead of native font hinting. Improves stem width consistency for fonts with weak or no native TrueType hints.")
args = parser.parse_args()

GlyphProps = namedtuple("GlyphProps", ["width", "height", "advance_x", "left", "top", "data_length", "data_offset", "code_point"])

font_stack = [freetype.Face(f) for f in args.fontstack]
is2Bit = args.is2Bit
size = args.size
font_name = args.name
load_flags = freetype.FT_LOAD_RENDER
if args.force_autohint:
    load_flags |= freetype.FT_LOAD_FORCE_AUTOHINT

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

# --- Kerning pair extraction ---
# Modern fonts store kerning in the OpenType GPOS table, which FreeType's
# get_kerning() does not read. We use fonttools to parse both the legacy
# kern table and the GPOS 'kern' feature (PairPos lookups, including
# Extension wrappers).

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

def _extract_pairpos_subtable(subtable, glyph_to_cp, raw_kern):
    """Extract kerning from a PairPos subtable (Format 1 or 2)."""
    if subtable.Format == 1:
        # Individual pairs
        for i, coverage_glyph in enumerate(subtable.Coverage.glyphs):
            if coverage_glyph not in glyph_to_cp:
                continue
            pair_set = subtable.PairSet[i]
            for pvr in pair_set.PairValueRecord:
                if pvr.SecondGlyph not in glyph_to_cp:
                    continue
                xa = 0
                if hasattr(pvr, 'Value1') and pvr.Value1:
                    xa = getattr(pvr.Value1, 'XAdvance', 0) or 0
                if xa != 0:
                    key = (coverage_glyph, pvr.SecondGlyph)
                    raw_kern[key] = raw_kern.get(key, 0) + xa
    elif subtable.Format == 2:
        # Class-based pairs
        class_def1 = subtable.ClassDef1.classDefs if subtable.ClassDef1 else {}
        class_def2 = subtable.ClassDef2.classDefs if subtable.ClassDef2 else {}
        coverage_set = set(subtable.Coverage.glyphs)
        for left_glyph in glyph_to_cp:
            if left_glyph not in coverage_set:
                continue
            c1 = class_def1.get(left_glyph, 0)
            if c1 >= len(subtable.Class1Record):
                continue
            class1_rec = subtable.Class1Record[c1]
            for right_glyph in glyph_to_cp:
                c2 = class_def2.get(right_glyph, 0)
                if c2 >= len(class1_rec.Class2Record):
                    continue
                c2_rec = class1_rec.Class2Record[c2]
                xa = 0
                if hasattr(c2_rec, 'Value1') and c2_rec.Value1:
                    xa = getattr(c2_rec.Value1, 'XAdvance', 0) or 0
                if xa != 0:
                    key = (left_glyph, right_glyph)
                    raw_kern[key] = raw_kern.get(key, 0) + xa

def extract_kerning_fonttools(font_path, codepoints, ppem):
    """Extract kerning pairs from a font file using fonttools.

    Returns dict of {(leftCp, rightCp): pixel_adjust} for the given
    codepoints.  Values are scaled from font design units to integer
    pixels at ppem.
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

    # Collect raw kerning values in font design units
    raw_kern = {}  # (left_glyph_name, right_glyph_name) -> design_units

    # 1. Legacy kern table
    if 'kern' in font:
        for subtable in font['kern'].kernTables:
            if hasattr(subtable, 'kernTable'):
                for (lg, rg), val in subtable.kernTable.items():
                    if lg in glyph_to_cp and rg in glyph_to_cp:
                        raw_kern[(lg, rg)] = raw_kern.get((lg, rg), 0) + val

    # 2. GPOS 'kern' feature
    if 'GPOS' in font:
        gpos = font['GPOS'].table
        kern_lookup_indices = set()
        if gpos.FeatureList:
            for fr in gpos.FeatureList.FeatureRecord:
                if fr.FeatureTag == 'kern':
                    kern_lookup_indices.update(fr.Feature.LookupListIndex)
        for li in kern_lookup_indices:
            lookup = gpos.LookupList.Lookup[li]
            for st in lookup.SubTable:
                actual = st
                # Unwrap Extension (lookup type 9) wrappers
                if lookup.LookupType == 9 and hasattr(st, 'ExtSubTable'):
                    actual = st.ExtSubTable
                if hasattr(actual, 'Format'):
                    _extract_pairpos_subtable(actual, glyph_to_cp, raw_kern)

    font.close()

    # Scale design-unit values to pixels
    scale = ppem / units_per_em
    result = {}  # (leftCp, rightCp) -> adjust
    for (lg, rg), du in raw_kern.items():
        lcp = glyph_to_cp[lg]
        rcp = glyph_to_cp[rg]
        adjust = int(math.floor(du * scale))
        if adjust != 0:
            adjust = max(-128, min(127, adjust))
            result[(lcp, rcp)] = adjust
    return result

# The ppem used by the existing glyph rasterization:
#   face.set_char_size(size << 6, size << 6, 150, 150)
# means size_pt at 150 DPI -> ppem = size * 150 / 72
ppem = size * 150.0 / 72.0

kern_map = {}  # (leftCp, rightCp) -> adjust
for face_idx, cps in face_idx_cps.items():
    font_path = args.fontstack[face_idx]
    kern_map.update(extract_kerning_fonttools(font_path, cps, ppem))

print(f"kerning: {len(kern_map)} pairs extracted", file=sys.stderr)

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

    if kern_left_class_count > 255 or kern_right_class_count > 255:
        print(f"WARNING: kerning class count exceeds uint8_t range "
              f"(left={kern_left_class_count}, right={kern_right_class_count})",
              file=sys.stderr)

    # Build the class x class matrix
    kern_matrix = [0] * (kern_left_class_count * kern_right_class_count)
    for (lcp, rcp), adjust in kern_map.items():
        lc = left_class_map[lcp] - 1
        rc = right_class_map[rcp] - 1
        kern_matrix[lc * kern_right_class_count + rc] = adjust

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

# Build groups for compression
if compress:
    # Script-based grouping: glyphs that co-occur in typical text rendering
    # are grouped together for efficient LRU caching on the embedded target.
    # Since glyphs are in codepoint order, glyphs in the same Unicode block
    # are contiguous in the array and form natural groups.
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
        (0xFB00, 0xFB06),   # Alphabetic Presentation Forms (ligatures)
        (0xFFFD, 0xFFFD),   # Replacement Character
    ]

    def get_script_group(code_point):
        for i, (start, end) in enumerate(SCRIPT_GROUP_RANGES):
            if start <= code_point <= end:
                return i
        return -1

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
    compressed_groups = []  # list of (compressed_bytes, uncompressed_size, glyph_count, first_glyph_index)
    compressed_bitmap_data = []
    compressed_offset = 0

    # Also build modified glyph props with within-group offsets
    modified_glyph_props = list(glyph_props)

    for first_idx, count in groups:
        # Concatenate bitmap data for this group
        group_data = b''
        for gi in range(first_idx, first_idx + count):
            props, packed = all_glyphs[gi]
            # Update glyph's dataOffset to be within-group offset
            within_group_offset = len(group_data)
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
            group_data += packed

        # Compress with raw DEFLATE (no zlib/gzip header)
        compressor = zlib.compressobj(level=9, wbits=-15)
        compressed = compressor.compress(group_data) + compressor.flush()

        compressed_groups.append((compressed, len(group_data), count, first_idx))
        compressed_bitmap_data.extend(compressed)
        compressed_offset += len(compressed)

    glyph_props = modified_glyph_props
    total_compressed = len(compressed_bitmap_data)
    total_uncompressed = len(glyph_data)
    print(f"// Compression: {total_uncompressed} -> {total_compressed} bytes ({100*total_compressed/total_uncompressed:.1f}%), {len(groups)} groups", file=sys.stderr)

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

def cp_label(cp):
    if cp == 0x5C:
        return '<backslash>'
    return chr(cp) if 0x20 < cp < 0x7F else f'U+{cp:04X}'

print(f"static const EpdGlyph {font_name}Glyphs[] = {{")
for i, g in enumerate(glyph_props):
    print ("    { " + ", ".join([f"{a}" for a in list(g[:-1])]),"},", f"// {cp_label(g.code_point)}")
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

if kern_map:
    print(f"static const EpdKernClassEntry {font_name}KernLeftClasses[] = {{")
    for cp, cls in kern_left_classes:
        print(f"    {{ 0x{cp:04X}, {cls} }}, // {cp_label(cp)}")
    print("};\n")

    print(f"static const EpdKernClassEntry {font_name}KernRightClasses[] = {{")
    for cp, cls in kern_right_classes:
        print(f"    {{ 0x{cp:04X}, {cls} }}, // {cp_label(cp)}")
    print("};\n")

    print(f"static const int8_t {font_name}KernMatrix[] = {{")
    for row in range(kern_left_class_count):
        row_start = row * kern_right_class_count
        row_vals = kern_matrix[row_start:row_start + kern_right_class_count]
        print("    " + ", ".join(f"{v:4d}" for v in row_vals) + ",")
    print("};\n")

if ligature_pairs:
    print(f"static const EpdLigaturePair {font_name}LigaturePairs[] = {{")
    for packed_pair, lig_cp in ligature_pairs:
        print(f"    {{ 0x{packed_pair:08X}, 0x{lig_cp:04X} }}, // {cp_label(packed_pair >> 16)} {cp_label(packed_pair & 0xFFFF)} -> {cp_label(lig_cp)}")
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
    print(f"    nullptr,")
    print(f"    0,")
if kern_map:
    print(f"    {font_name}KernLeftClasses,")
    print(f"    {font_name}KernRightClasses,")
    print(f"    {font_name}KernMatrix,")
    print(f"    {len(kern_left_classes)},")
    print(f"    {len(kern_right_classes)},")
    print(f"    {kern_left_class_count},")
    print(f"    {kern_right_class_count},")
else:
    print(f"    nullptr,")
    print(f"    nullptr,")
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
