#!/usr/bin/env python3
"""Generate .cpfont binary files for SD card font loading.

Outputs binary .cpfont files containing glyph metadata and uncompressed
2-bit bitmaps, matching the EpdFontData/EpdGlyph/EpdUnicodeInterval struct
layout on the ESP32-C3 (little-endian, RISC-V).

Usage:
    # Single file with specific presets
    python fontconvert_sdcard.py \\
      --intervals latin-ext,greek,cyrillic \\
      --size 14 --style regular --2bit \\
      NotoSans-Regular.ttf \\
      -o NotoSansExt_14_regular.cpfont

    # All 4 sizes at once
    python fontconvert_sdcard.py \\
      --intervals cjk \\
      --sizes 12,14,16,18 --style regular --2bit \\
      NotoSansCJKsc-Regular.otf \\
      --output-dir NotoSansCJK/

    # Legacy positional-argument mode (backward compat)
    python fontconvert_sdcard.py NotoSansCJK_14_regular 14 NotoSansCJKsc-Regular.otf --2bit
"""

import freetype
import struct
import sys
import os
import math
import argparse
from collections import namedtuple

from fontTools.ttLib import TTFont

# --- Unicode interval presets ---

INTERVAL_PRESETS = {
    "ascii":       [(0x0020, 0x007E)],
    "latin1":      [(0x0080, 0x00FF)],
    "latin-ext":   [(0x0020, 0x007E), (0x0080, 0x00FF), (0x0100, 0x024F),
                    (0x1E00, 0x1EFF), (0x2000, 0x206F)],
    "greek":       [(0x0370, 0x03FF), (0x1F00, 0x1FFF)],
    "cyrillic":    [(0x0400, 0x04FF), (0x0500, 0x052F)],
    "georgian":    [(0x10A0, 0x10FF), (0x2D00, 0x2D2F)],
    "armenian":    [(0x0530, 0x058F)],
    "ethiopic":    [(0x1200, 0x137F), (0x1380, 0x139F), (0x2D80, 0x2DDF)],
    "vietnamese":  [(0x01A0, 0x01B0), (0x1EA0, 0x1EF9)],
    "punctuation": [(0x2000, 0x206F)],
    "cjk":         [(0x3000, 0x303F), (0x3040, 0x309F), (0x30A0, 0x30FF),
                    (0x4E00, 0x9FFF), (0xF900, 0xFAFF), (0xFF00, 0xFFEF)],
    "hangul":      [(0xAC00, 0xD7AF), (0x1100, 0x11FF), (0x3130, 0x318F)],
    # Matches the built-in font intervals from fontconvert.py exactly
    "builtin":     [(0x0000, 0x007F), (0x0080, 0x00FF), (0x0100, 0x017F),
                    (0x01A0, 0x01A1), (0x01AF, 0x01B0), (0x01C4, 0x021F),
                    (0x0300, 0x036F), (0x0400, 0x04FF),
                    (0x1EA0, 0x1EF9), (0x2000, 0x206F), (0x20A0, 0x20CF),
                    (0x2070, 0x209F), (0x2190, 0x21FF), (0x2200, 0x22FF),
                    (0xFB00, 0xFB06)],
}

# Legacy CJK intervals (for backward compat with positional-arg mode)
LEGACY_CJK_INTERVALS = [
    (0x0020, 0x007E), (0x0080, 0x00FF), (0x2000, 0x206F),
    (0x3000, 0x303F), (0x3040, 0x309F), (0x30A0, 0x30FF),
    (0x4E00, 0x9FFF), (0xF900, 0xFAFF), (0xFF00, 0xFFEF),
    (0xFFFD, 0xFFFD),
]


def resolve_intervals(preset_str):
    """Resolve comma-separated preset names into a merged, sorted, deduplicated interval list."""
    all_intervals = []
    for name in preset_str.split(","):
        name = name.strip().lower()
        if name not in INTERVAL_PRESETS:
            print(f"Error: unknown interval preset '{name}'", file=sys.stderr)
            print(f"Available presets: {', '.join(sorted(INTERVAL_PRESETS.keys()))}", file=sys.stderr)
            sys.exit(1)
        all_intervals.extend(INTERVAL_PRESETS[name])

    # Always add replacement character
    all_intervals.append((0xFFFD, 0xFFFD))

    # Sort and merge overlapping/adjacent intervals
    all_intervals.sort()
    merged = []
    for start, end in all_intervals:
        if merged and start <= merged[-1][1] + 1:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return merged


GlyphProps = namedtuple("GlyphProps", [
    "width", "height", "advance_x", "left", "top", "data_length", "data_offset", "code_point"
])


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

    # Scale design-unit kerning values to 4.4 fixed-point pixels.
    scale = ppem / units_per_em
    result = {}  # (leftCp, rightCp) -> 4.4 fixed-point adjust
    for (lg, rg), du in raw_kern.items():
        lcp = glyph_to_cp[lg]
        rcp = glyph_to_cp[rg]
        adjust = fp4_from_design_units(du, scale)
        if adjust != 0:
            result[(lcp, rcp)] = adjust
    return result


def derive_kern_classes(kern_map):
    """Derive class-based kerning from a pair map.

    Returns (kern_left_classes, kern_right_classes, kern_matrix,
             kern_left_class_count, kern_right_class_count) where:
    - kern_left_classes: sorted list of (codepoint, classId) tuples
    - kern_right_classes: sorted list of (codepoint, classId) tuples
    - kern_matrix: flat list of int8 values (left_class_count * right_class_count)
    - kern_left_class_count: number of distinct left classes
    - kern_right_class_count: number of distinct right classes
    """
    if not kern_map:
        return [], [], [], 0, 0

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

    return (kern_left_classes, kern_right_classes, kern_matrix,
            kern_left_class_count, kern_right_class_count)


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
    codepoints_set = set(codepoints)
    filtered = {}
    for seq, lig_cp in raw_ligatures.items():
        if lig_cp not in codepoints_set:
            continue
        if all(cp in codepoints_set for cp in seq):
            filtered[seq] = lig_cp

    # Decompose into chained pairs
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


def generate_cpfont(fontfile, size, intervals, output_path, is2bit=True, force_autohint=False):
    """Generate a single .cpfont file."""
    face = freetype.Face(fontfile)
    load_flags = freetype.FT_LOAD_RENDER
    if force_autohint:
        load_flags |= freetype.FT_LOAD_FORCE_AUTOHINT

    def load_glyph(code_point):
        glyph_index = face.get_char_index(code_point)
        if glyph_index > 0:
            face.load_glyph(glyph_index, load_flags)
            return face
        return None

    # Validate intervals: remove codepoints not present in the font
    print(f"  Validating intervals against font...", file=sys.stderr)
    validated_intervals = []
    for i_start, i_end in intervals:
        start = i_start
        for code_point in range(i_start, i_end + 1):
            f = load_glyph(code_point)
            if f is None:
                if start < code_point:
                    validated_intervals.append((start, code_point - 1))
                start = code_point + 1
        if start <= i_end:
            validated_intervals.append((start, i_end))

    intervals = validated_intervals
    total_glyphs = sum(end - start + 1 for start, end in intervals)
    print(f"  Validated: {len(intervals)} intervals, {total_glyphs} glyphs", file=sys.stderr)

    # Set font size at 150 DPI (matching fontconvert.py)
    face.set_char_size(size << 6, size << 6, 150, 150)

    # Rasterize all glyphs
    total_bitmap_size = 0
    all_glyphs = []

    for i_start, i_end in intervals:
        for code_point in range(i_start, i_end + 1):
            f = load_glyph(code_point)
            if f is None:
                glyph = GlyphProps(0, 0, 0, 0, 0, 0, total_bitmap_size, code_point)
                all_glyphs.append((glyph, b''))
                continue

            bitmap = f.glyph.bitmap

            # Build 4-bit greyscale bitmap (same logic as fontconvert.py)
            pixels4g = []
            px = 0
            for i, v in enumerate(bitmap.buffer):
                x = i % bitmap.width
                if x % 2 == 0:
                    px = (v >> 4)
                else:
                    px = px | (v & 0xF0)
                    pixels4g.append(px)
                    px = 0
                if x == bitmap.width - 1 and bitmap.width % 2 > 0:
                    pixels4g.append(px)
                    px = 0

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

            packed = bytes(pixels2b)
            glyph = GlyphProps(
                width=bitmap.width,
                height=bitmap.rows,
                advance_x=fp4_from_ft16_16(f.glyph.linearHoriAdvance),
                left=f.glyph.bitmap_left,
                top=f.glyph.bitmap_top,
                data_length=len(packed),
                data_offset=total_bitmap_size,
                code_point=code_point,
            )
            total_bitmap_size += len(packed)
            all_glyphs.append((glyph, packed))

    # Get font metrics from pipe character (same heuristic as fontconvert.py)
    load_glyph(ord('|'))

    advanceY = norm_ceil(face.size.height)
    ascender = norm_ceil(face.size.ascender)
    descender = norm_floor(face.size.descender)

    print(f"  Font metrics: advanceY={advanceY}, ascender={ascender}, descender={descender}", file=sys.stderr)
    print(f"  Total bitmap size: {total_bitmap_size} bytes ({total_bitmap_size / 1024:.1f} KB)", file=sys.stderr)

    # --- Extract kerning and ligatures ---
    ppem = size * 150.0 / 72.0
    all_cps = set(g.code_point for g, _ in all_glyphs)

    kern_map = extract_kerning_fonttools(fontfile, all_cps, ppem)
    print(f"  Kerning: {len(kern_map)} pairs extracted", file=sys.stderr)

    (kern_left_classes, kern_right_classes, kern_matrix,
     kern_left_class_count, kern_right_class_count) = derive_kern_classes(kern_map)

    if kern_map:
        matrix_size = kern_left_class_count * kern_right_class_count
        entries_size = (len(kern_left_classes) + len(kern_right_classes)) * 3
        print(f"  Kerning classes: {kern_left_class_count} left, {kern_right_class_count} right, "
              f"{matrix_size + entries_size} bytes", file=sys.stderr)

    ligature_pairs = extract_ligatures_fonttools(fontfile, all_cps)
    if len(ligature_pairs) > 255:
        print(f"  WARNING: {len(ligature_pairs)} ligature pairs exceeds uint8_t max (255), truncating",
              file=sys.stderr)
        ligature_pairs = ligature_pairs[:255]
    print(f"  Ligatures: {len(ligature_pairs)} pairs", file=sys.stderr)

    # Build binary .cpfont file (v3: advanceX is 12.4 fp, kern is 4.4 fp)
    MAGIC = b"CPFONT\x00\x00"
    VERSION = 3
    flags = 1 if is2bit else 0

    header = struct.pack("<8sHHIIBhhHHBBB",
                         MAGIC, VERSION, flags,
                         len(intervals), len(all_glyphs),
                         advanceY & 0xFF, ascender, descender,
                         len(kern_left_classes), len(kern_right_classes),
                         kern_left_class_count, kern_right_class_count,
                         len(ligature_pairs))
    assert len(header) == 32

    # Intervals section
    intervals_data = bytearray()
    offset = 0
    for i_start, i_end in intervals:
        intervals_data += struct.pack("<III", i_start, i_end, offset)
        offset += i_end - i_start + 1

    # Glyph section — must match EpdGlyph struct layout (16 bytes, little-endian):
    #   uint8_t  width        (offset 0)
    #   uint8_t  height       (offset 1)
    #   uint16_t advanceX     (offset 2)  — 12.4 fixed-point
    #   int16_t  left         (offset 4)
    #   int16_t  top          (offset 6)
    #   uint16_t dataLength   (offset 8)
    #   [2 pad]               (offset 10) — for uint32_t alignment
    #   uint32_t dataOffset   (offset 12)
    GLYPH_STRUCT_FORMAT = "<BBHhhH2xI"
    assert struct.calcsize(GLYPH_STRUCT_FORMAT) == 16

    glyphs_data = bytearray()
    for glyph, packed in all_glyphs:
        glyphs_data += struct.pack(GLYPH_STRUCT_FORMAT,
                                   glyph.width, glyph.height, glyph.advance_x,
                                   glyph.left, glyph.top,
                                   glyph.data_length, glyph.data_offset)

    # Kern left classes section: uint16_t codepoint + uint8_t classId = 3 bytes each
    kern_left_data = bytearray()
    for cp, cls in kern_left_classes:
        kern_left_data += struct.pack("<HB", cp, cls)

    # Kern right classes section: same format
    kern_right_data = bytearray()
    for cp, cls in kern_right_classes:
        kern_right_data += struct.pack("<HB", cp, cls)

    # Kern matrix section: flat int8_t array
    kern_matrix_data = bytearray()
    if kern_matrix:
        kern_matrix_data = bytearray(struct.pack(f"<{len(kern_matrix)}b", *kern_matrix))

    # Ligature pairs section: uint32_t pair + uint32_t ligatureCp = 8 bytes each
    ligature_data = bytearray()
    for packed_pair, lig_cp in ligature_pairs:
        ligature_data += struct.pack("<II", packed_pair, lig_cp)

    # Bitmap section
    bitmap_data = bytearray()
    for glyph, packed in all_glyphs:
        bitmap_data += packed
    assert len(bitmap_data) == total_bitmap_size

    # Write output file
    os.makedirs(os.path.dirname(output_path) if os.path.dirname(output_path) else ".", exist_ok=True)
    with open(output_path, "wb") as f:
        f.write(header)
        f.write(intervals_data)
        f.write(glyphs_data)
        f.write(kern_left_data)
        f.write(kern_right_data)
        f.write(kern_matrix_data)
        f.write(ligature_data)
        f.write(bitmap_data)

    kern_lig_size = len(kern_left_data) + len(kern_right_data) + len(kern_matrix_data) + len(ligature_data)
    total_file_size = len(header) + len(intervals_data) + len(glyphs_data) + kern_lig_size + len(bitmap_data)
    print(f"  Output: {output_path}", file=sys.stderr)
    print(f"    Header:    {len(header)} bytes", file=sys.stderr)
    print(f"    Intervals: {len(intervals_data)} bytes ({len(intervals)} intervals)", file=sys.stderr)
    print(f"    Glyphs:    {len(glyphs_data)} bytes ({len(all_glyphs)} glyphs)", file=sys.stderr)
    print(f"    Kern/Lig:  {kern_lig_size} bytes", file=sys.stderr)
    print(f"    Bitmaps:   {len(bitmap_data)} bytes", file=sys.stderr)
    print(f"    Total:     {total_file_size} bytes ({total_file_size / 1024 / 1024:.2f} MB)", file=sys.stderr)
    return total_file_size


def main():
    parser = argparse.ArgumentParser(
        description="Generate .cpfont files for SD card font loading.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=f"Available interval presets: {', '.join(sorted(INTERVAL_PRESETS.keys()))}"
    )

    # New-style arguments
    parser.add_argument("fontfile", action="store",
                        help="Path to the font file (e.g., .otf or .ttf).")
    parser.add_argument("--intervals", dest="intervals",
                        help="Comma-separated interval presets (e.g., 'latin-ext,greek,cyrillic').")
    parser.add_argument("--size", type=int, dest="size",
                        help="Single font size to generate.")
    parser.add_argument("--sizes", dest="sizes",
                        help="Comma-separated sizes (e.g., '12,14,16,18').")
    parser.add_argument("--style", dest="style", default="regular",
                        choices=["regular", "bold", "italic", "bolditalic"],
                        help="Font style (default: regular).")
    parser.add_argument("--name", dest="name",
                        help="Font family name for output filenames (default: derived from font filename).")
    parser.add_argument("--2bit", dest="is2Bit", action="store_true", default=True,
                        help="Generate 2-bit greyscale bitmap (default, always on).")
    parser.add_argument("--force-autohint", dest="force_autohint", action="store_true",
                        help="Force FreeType auto-hinter instead of native font hinting.")
    parser.add_argument("-o", "--output", dest="output",
                        help="Output file path (for single-size mode).")
    parser.add_argument("--output-dir", dest="output_dir",
                        help="Output directory (for multi-size mode). Files named <Name>_<size>_<style>.cpfont.")
    parser.add_argument("--list-presets", action="store_true",
                        help="List available interval presets and exit.")

    # Legacy positional arguments for backward compatibility
    parser.add_argument("legacy_args", nargs="*",
                        help=argparse.SUPPRESS)

    args = parser.parse_args()

    if args.list_presets:
        print("Available interval presets:")
        for name, ranges in sorted(INTERVAL_PRESETS.items()):
            total = sum(e - s + 1 for s, e in ranges)
            print(f"  {name:15s}  {len(ranges)} range(s), ~{total} codepoints")
        sys.exit(0)

    # Detect legacy mode: if fontfile is not a file but legacy_args has items
    # Legacy: fontconvert_sdcard.py <name> <size> <fontfile> --2bit
    fontfile = args.fontfile
    if args.legacy_args and len(args.legacy_args) >= 2 and not args.intervals:
        # Legacy positional mode: name size fontfile
        legacy_name = fontfile
        legacy_size = int(args.legacy_args[0])
        fontfile = args.legacy_args[1]
        intervals = LEGACY_CJK_INTERVALS
        output_path = args.output if args.output else f"{legacy_name}.cpfont"
        print(f"Generating {output_path} (size {legacy_size}, legacy CJK intervals)...", file=sys.stderr)
        generate_cpfont(fontfile, legacy_size, intervals, output_path,
                        is2bit=True, force_autohint=args.force_autohint)
        return

    # New-style mode
    if not args.intervals:
        print("Error: --intervals is required (e.g., --intervals latin-ext,greek,cyrillic)", file=sys.stderr)
        print(f"Available presets: {', '.join(sorted(INTERVAL_PRESETS.keys()))}", file=sys.stderr)
        sys.exit(1)

    intervals = resolve_intervals(args.intervals)

    # Determine sizes
    if args.sizes:
        sizes = [int(s.strip()) for s in args.sizes.split(",")]
    elif args.size:
        sizes = [args.size]
    else:
        print("Error: --size or --sizes is required", file=sys.stderr)
        sys.exit(1)

    # Determine font name
    if args.name:
        font_name = args.name
    else:
        # Derive from font filename: NotoSans-Regular.ttf -> NotoSans
        base = os.path.splitext(os.path.basename(fontfile))[0]
        # Remove style suffix (-Regular, -Bold, etc.)
        for suffix in ["-Regular", "-Bold", "-Italic", "-BoldItalic",
                       "-regular", "-bold", "-italic", "-bolditalic"]:
            if base.endswith(suffix):
                base = base[:-len(suffix)]
                break
        font_name = base

    style = args.style

    if len(sizes) == 1 and args.output:
        # Single file mode
        output_path = args.output
        print(f"Generating {output_path} (size {sizes[0]}, style {style})...", file=sys.stderr)
        generate_cpfont(fontfile, sizes[0], intervals, output_path,
                        is2bit=True, force_autohint=args.force_autohint)
    else:
        # Multi-size mode
        output_dir = args.output_dir if args.output_dir else f"{font_name}/"
        total_size = 0
        for sz in sizes:
            filename = f"{font_name}_{sz}_{style}.cpfont"
            output_path = os.path.join(output_dir, filename)
            print(f"Generating {output_path} (size {sz}, style {style})...", file=sys.stderr)
            total_size += generate_cpfont(fontfile, sz, intervals, output_path,
                                          is2bit=True, force_autohint=args.force_autohint)
        print(f"\nTotal: {len(sizes)} files, {total_size / 1024 / 1024:.2f} MB", file=sys.stderr)


if __name__ == "__main__":
    main()
