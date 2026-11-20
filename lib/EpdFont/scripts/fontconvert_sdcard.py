#!/usr/bin/env python3
"""Generate .cpfont binary files for SD card font loading.

Font licenses and copyright notices are in licenses/fonts.txt.

Outputs binary .cpfont files containing glyph metadata and uncompressed
2-bit (default, flags=0x0001) or 4-bit (--4bit, flags=0x0002) bitmaps,
matching the EpdFontData/EpdGlyph/EpdUnicodeInterval struct layout on the
ESP32-C3 (little-endian, RISC-V).

Usage:
    # Single file with specific presets
    python fontconvert_sdcard.py \\
      --intervals latin-ext,greek,cyrillic \\
      --size 14 --style regular \\
      NotoSans-Regular.ttf \\
      -o NotoSansExt_14.cpfont

    # All 4 sizes at once
    python fontconvert_sdcard.py \\
      --intervals cjk \\
      --sizes 12,14,16,18 --style regular \\
      NotoSansCJKsc-Regular.otf \\
      --output-dir NotoSansCJK/

"""

from __future__ import annotations

import ctypes
import struct
import sys
import os
import re
import math
import argparse
from collections import namedtuple

from cpfont_version import CPFONT_VERSION
import shaping_blob

CPFONT_FLAG_2BIT = 0x0001
CPFONT_FLAG_4BIT = 0x0002

# --- Unicode interval presets ---

INTERVAL_PRESETS = {
    "ascii":       [(0x0020, 0x007E)],
    "latin1":      [(0x0080, 0x00FF)],
    "latin-ext":   [(0x0020, 0x007E), (0x0080, 0x00FF), (0x0100, 0x024F),
                    (0x02B0, 0x02FF), (0x0300, 0x036F), (0x1E00, 0x1EFF), (0x2000, 0x206F),
                    (0x20A0, 0x20CF), (0x2100, 0x214F), (0x2212, 0x2212),
                    (0xFB00, 0xFB06)],
    "greek":       [(0x0370, 0x03FF), (0x1F00, 0x1FFF)],
    "cyrillic":    [(0x0400, 0x04FF), (0x0500, 0x052F)],
    "hebrew":      [(0x0590, 0x05FF), (0xFB1D, 0xFB4F)],
    "arabic":      [(0x0600, 0x06FF), (0x0750, 0x077F), (0x08A0, 0x08FF),
                    (0xFB50, 0xFDF9), (0xFE70, 0xFEFF)],
    "georgian":    [(0x10A0, 0x10FF), (0x2D00, 0x2D2F)],
    "armenian":    [(0x0530, 0x058F)],
    "ethiopic":    [(0x1200, 0x137F), (0x1380, 0x139F), (0x2D80, 0x2DDF)],
    "vietnamese":  [(0x01A0, 0x01B0), (0x1EA0, 0x1EF9)],
    "ipa-chars":   [(0x0250, 0x02AF), (0x02B0, 0x02FF)],
    "punctuation": [(0x2000, 0x206F)],
    "cjk":         [(0x3000, 0x303F), (0x3040, 0x309F), (0x30A0, 0x30FF),
                    (0x4E00, 0x9FFF), (0xF900, 0xFAFF), (0xFF00, 0xFFEF)],
    "hangul":      [(0xAC00, 0xD7AF), (0x1100, 0x11FF), (0x3130, 0x318F)],
    "cherokee":    [(0x13A0, 0x13FF), (0xAB70, 0xABBF)],
    "tifinagh":    [(0x2D30, 0x2D7F)],
    # One preset per Indic script (devanagari, bengali, ..., sinhala; see
    # shaping_blob.SCRIPTS) is added below.
    # Symbol blocks commonly seen in scifi/popsci/literary fiction.

    "symbols":     [(0x2070, 0x209F), (0x20A0, 0x20CF), (0x2150, 0x218F),
                    (0x2190, 0x21FF), (0x2200, 0x22FF), (0x2500, 0x257F),
                    (0x25A0, 0x25FF), (0x2600, 0x26FF), (0x2700, 0x27BF)],
    # Composite preset for English-language literary fiction including scifi/popsci.
    # Greek for physics terms, math operators, geometric shapes, uncommon
    # dialogue punctuation, CJK quote marks, miscellaneous symbols (♪♫♬), dingbats.
    "reading":     [(0x0020, 0x024F), (0x02B0, 0x02FF), (0x0300, 0x036F), (0x0370, 0x03FF),
                    (0x0400, 0x04FF), (0x1E00, 0x1EFF), (0x2000, 0x206F),
                    (0x2070, 0x209F), (0x20A0, 0x20CF), (0x2150, 0x218F),
                    (0x2190, 0x21FF), (0x2200, 0x22FF), (0x2500, 0x257F),
                    (0x25A0, 0x25FF), (0x2600, 0x26FF), (0x2700, 0x27BF),
                    (0x2900, 0x29FF), (0x2E00, 0x2E7F), (0x3000, 0x303F),
                    (0xFB00, 0xFB06)],
    # Matches the built-in font intervals from fontconvert.py exactly
    "builtin":     [(0x0000, 0x007F), (0x0080, 0x00FF), (0x0100, 0x017F),
                    (0x01A0, 0x01A1), (0x01AF, 0x01B0), (0x01C4, 0x021F),
                    (0x0300, 0x036F), (0x0400, 0x04FF),
                    (0x1EA0, 0x1EF9), (0x2000, 0x206F), (0x20A0, 0x20CF),
                    (0x2070, 0x209F), (0x2190, 0x21FF), (0x2200, 0x22FF),
                    (0xFB00, 0xFB06)],
}

# Indic script presets: the script's block plus the dandas and joiners its text
# uses. Fonts with GSUB also get a shaping section (conjuncts, reph,
# positioned marks) — see shaping_blob.py.
INTERVAL_PRESETS.update({
    name: [(script.first, script.first + 0x7F), *shaping_blob.SHARED_INTERVALS]
    for name, script in shaping_blob.SCRIPTS.items()
})

# Regex for parsing unnamed hex range intervals: (0xSTART-0xEND)
_HEX_RANGE_PATTERN = re.compile(r'^\(0x([0-9a-fA-F]+)-0x([0-9a-fA-F]+)\)$')

def parse_hex_range(s: str) -> tuple[int, int] | None:
    match = _HEX_RANGE_PATTERN.fullmatch(s)
    if not match:
        return None

    start_hex, end_hex = match.groups()
    start, end = int(start_hex, 16), int(end_hex, 16)

    # Validating Unicode range bounds.
    if start > end or end > 0x10FFFF:
        return None
    return start, end


def resolve_intervals(preset_str):
    """Resolve comma-separated preset names into a merged, sorted, deduplicated interval list."""
    all_intervals = []
    for name in preset_str.split(","):
        name = name.strip().lower()
        unnamed_interval = parse_hex_range(name)
        if name not in INTERVAL_PRESETS and unnamed_interval is None:
            print(f"Error: unknown interval preset '{name}'", file=sys.stderr)
            print(f"Available presets: {', '.join(sorted(INTERVAL_PRESETS.keys()))}", file=sys.stderr)
            print("You can also specify unnamed hex ranges like (0x2100-0x214F)", file=sys.stderr)
            sys.exit(1)

        if unnamed_interval is not None:
            all_intervals.append(unnamed_interval)
        else:
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

# Intermediate data from rasterizing one font style
StyleRasterData = namedtuple("StyleRasterData", [
    "style_id",                # 0=regular, 1=bold, 2=italic, 3=bolditalic
    "intervals",               # validated intervals [(start, end), ...]
    "all_glyphs",              # [(GlyphProps, packed_bytes), ...]
    "total_bitmap_size",       # int
    "advanceY", "ascender", "descender",
    "kern_left_classes", "kern_right_classes", "kern_matrix",
    "kern_left_class_count", "kern_right_class_count",
    "ligature_pairs",
    "shaping_section",         # bytes of the trailing shaping section, or b"" when none
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


def extract_ligature_glyph_indices_fonttools(font_path):
    """Map standard ligature codepoints without a cmap entry to their glyph IDs.

    Most fonts use unencoded glyphs such as ``f_f`` for standard ligatures.
    The reader represents those substitutions with the Unicode compatibility
    codepoints, so rasterize the GSUB target directly instead of falling back
    to another font for a codepoint that the primary cmap does not contain.
    """
    from fontTools.ttLib import TTFont

    font = TTFont(font_path)
    cmap = font.getBestCmap() or {}
    glyph_to_cp = {gname: cp for cp, gname in cmap.items()}
    glyph_indices = {gname: index for index, gname in enumerate(font.getGlyphOrder())}
    overrides = {}

    if 'GSUB' in font:
        gsub = font['GSUB'].table
        liga_lookup_indices = set()
        if gsub.FeatureList:
            for fr in gsub.FeatureList.FeatureRecord:
                if fr.FeatureTag in ('liga', 'rlig'):
                    liga_lookup_indices.update(fr.Feature.LookupListIndex)

        for li in liga_lookup_indices:
            lookup = gsub.LookupList.Lookup[li]
            for st in lookup.SubTable:
                actual = st.ExtSubTable if lookup.LookupType == 7 and hasattr(st, 'ExtSubTable') else st
                if not hasattr(actual, 'ligatures'):
                    continue
                for first_glyph, ligature_list in actual.ligatures.items():
                    if first_glyph not in glyph_to_cp:
                        continue
                    for lig in ligature_list:
                        if any(component not in glyph_to_cp for component in lig.Component):
                            continue
                        seq = tuple([glyph_to_cp[first_glyph]] + [glyph_to_cp[component] for component in lig.Component])
                        lig_cp = STANDARD_LIGATURE_MAP.get(seq)
                        if lig_cp is not None and lig_cp not in cmap:
                            overrides[lig_cp] = glyph_indices[lig.LigGlyph]

    font.close()
    return overrides


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
        # Class-based pairs — iterate by class, not by glyph, to avoid
        # O(glyphs²) explosion for CJK fonts with many requested glyphs.
        class_def1 = subtable.ClassDef1.classDefs if subtable.ClassDef1 else {}
        class_def2 = subtable.ClassDef2.classDefs if subtable.ClassDef2 else {}
        coverage_set = set(subtable.Coverage.glyphs)

        # Build reverse mappings: class_id -> list of glyph names
        left_by_class = {}   # only glyphs in coverage AND glyph_to_cp
        for glyph in glyph_to_cp:
            if glyph not in coverage_set:
                continue
            c1 = class_def1.get(glyph, 0)
            left_by_class.setdefault(c1, []).append(glyph)

        right_by_class = {}  # all glyphs in glyph_to_cp
        for glyph in glyph_to_cp:
            c2 = class_def2.get(glyph, 0)
            right_by_class.setdefault(c2, []).append(glyph)

        # Iterate class pairs (typically << glyph pairs)
        for c1, class1_rec in enumerate(subtable.Class1Record):
            if c1 not in left_by_class:
                continue
            for c2, c2_rec in enumerate(class1_rec.Class2Record):
                xa = 0
                if hasattr(c2_rec, 'Value1') and c2_rec.Value1:
                    xa = getattr(c2_rec.Value1, 'XAdvance', 0) or 0
                if xa == 0:
                    continue
                if c2 not in right_by_class:
                    continue
                for lg in left_by_class[c1]:
                    for rg in right_by_class[c2]:
                        key = (lg, rg)
                        raw_kern[key] = raw_kern.get(key, 0) + xa


def extract_kerning_fonttools(font_path, codepoints, ppem):
    """Extract kerning pairs from a font file using fonttools.

    Returns dict of {(leftCp, rightCp): pixel_adjust} for the given
    codepoints.  Values are scaled from font design units to integer
    pixels at ppem.
    """
    from fontTools.ttLib import TTFont

    font = TTFont(font_path)
    units_per_em = font['head'].unitsPerEm
    cmap = font.getBestCmap() or {}

    # Build glyph_name -> [codepoints] map (preserves aliases where multiple
    # codepoints share a glyph, e.g. space/nbsp)
    glyph_to_cps = {}
    for cp in codepoints:
        gname = cmap.get(cp)
        if gname:
            glyph_to_cps.setdefault(gname, []).append(cp)
    # Flat dict for membership checks and subtable extraction (uses keys only)
    glyph_to_cp = glyph_to_cps

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
                # Unwrap Extension (lookup type 9) wrappers. After unwrapping,
                # `lookup.LookupType` is still 9, so we must look at the
                # *effective* type carried on the extension subtable to know
                # whether `actual` is a PairPos table.
                if lookup.LookupType == 9 and hasattr(st, 'ExtSubTable'):
                    actual = st.ExtSubTable
                effective_type = getattr(st, 'ExtensionLookupType', lookup.LookupType)
                if hasattr(actual, 'Format'):
                    # _extract_pairpos_subtable assumes a Type-2 (PairPos)
                    # subtable. Other lookup types reachable through the kern
                    # feature (cursive attachment, mark-to-mark, contextual,
                    # etc.) have a different shape and crash inside the
                    # extractor. Skip them with a debug note rather than
                    # aborting the whole build. Modern fonts often ship kern
                    # via Extension-wrapped PairPos, so checking the effective
                    # type instead of the outer type is what makes those
                    # lookups actually reach the extractor.
                    if effective_type == 2:
                        _extract_pairpos_subtable(actual, glyph_to_cp, raw_kern)
                    else:
                        print(f"  Debug: skipping unsupported GPOS kern lookupType="
                              f"{effective_type} (outer={lookup.LookupType}, Format={actual.Format})",
                              file=sys.stderr)

    font.close()

    # Scale design-unit kerning values to 4.4 fixed-point pixels.
    # Expand glyph aliases: if multiple codepoints share a glyph, emit kern
    # pairs for all codepoint combinations.
    scale = ppem / units_per_em
    result = {}  # (leftCp, rightCp) -> 4.4 fixed-point adjust
    for (lg, rg), du in raw_kern.items():
        adjust = fp4_from_design_units(du, scale)
        if adjust != 0:
            for lcp in glyph_to_cps[lg]:
                for rcp in glyph_to_cps[rg]:
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
              f"(left={kern_left_class_count}, right={kern_right_class_count}), "
              f"dropping kerning for this style",
              file=sys.stderr)
        return ([], [], [], 0, 0)

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
    from fontTools.ttLib import TTFont

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
    # in our generated glyph set, and all codepoints fit in 16 bits.
    #
    # The on-disk format packs each component as a uint16 (the 3+ chained
    # path packs `intermediate_cp << 16 | last_cp`, where `intermediate_cp`
    # is the lig_cp of the prefix). Dropping any seq with an SMP cp here —
    # plus any lig_cp > 0xFFFF — means every cp that reaches `packed = … <<
    # 16 | …` below is already 16-bit safe, including the chained path
    # (intermediate_cp = filtered[prefix] is filtered too).
    codepoints_set = set(codepoints)
    filtered = {}
    for seq, lig_cp in raw_ligatures.items():
        if lig_cp not in codepoints_set or lig_cp > 0xFFFF:
            continue
        if any(cp > 0xFFFF for cp in seq):
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

    # Sort by packed pair key — on-device lookup uses binary search
    pairs.sort(key=lambda p: p[0])
    return pairs



def build_quant_lut(bpp: int, gamma: float = 1.0, halo_clip: bool = False) -> bytes:
    """Build 256-entry 8-bit -> {2bpp (0..3) | 4bpp (0..15)} quantization LUT.

    At gamma == 1.0 and halo_clip == False, 2bpp keeps the legacy 64/128/192
    thresholds so default 2-bit .cpfont output stays byte-identical; 4bpp and
    non-unity gamma use reflectance-space mapping 1 - (1 - a/255)**gamma.
    When halo_clip is True, coverage <= 15/255 clips to 0 (and >= 240/255 clips
    to 15 in 4bpp mode).
    """
    if bpp not in (2, 4):
        raise ValueError(f"Unsupported bpp: {bpp}")
    if gamma <= 0.0 or not math.isfinite(gamma):
        raise ValueError(f"gamma must be finite and positive, got {gamma}")
    if bpp == 2 and gamma == 1.0:
        return bytes(0 if i < 64 else 1 if i < 128 else 2 if i < 192 else 3 for i in range(256))
    max_level = (1 << bpp) - 1
    out = bytearray(256)
    for i in range(1, 256):
        if halo_clip and i <= 15:
            out[i] = 0
            continue
        if halo_clip and bpp == 4 and i >= 240:
            out[i] = max_level
            continue
        lum_mapped = round(255.0 * (1.0 - ((1.0 - i / 255.0) ** gamma)))
        lum_mapped = max(0, min(255, lum_mapped))
        out[i] = (lum_mapped * max_level + 127) // 255
    return bytes(out)


def pack_bitmap_4b(buf, width: int, rows: int, pitch: int, lut4: bytes,
                   counter_guard: bool = False) -> bytes:
    """Pack 8-bit FreeType bitmap into contiguous MSB-nibble-first 4bpp: (width * rows + 1) // 2 bytes."""
    total = width * rows
    if total == 0:
        return b""
    abs_pitch = abs(pitch)
    vals = bytearray(total + (total & 1))
    idx = 0
    for y in range(rows):
        row_off = y * abs_pitch if pitch >= 0 else (rows - 1 - y) * abs_pitch
        for x in range(width):
            vals[idx] = lut4[buf[row_off + x]]
            idx += 1
    if counter_guard and ((width >= 3 and rows >= 2) or (width >= 2 and rows >= 3)):
        orig = bytes(vals[:total])
        for y in range(rows):
            r_off = y * width
            for x in range(width):
                p = r_off + x
                if 0 < orig[p] <= 2:
                    dark = (
                        (1 if x > 0 and orig[p - 1] >= 10 else 0)
                        + (1 if x + 1 < width and orig[p + 1] >= 10 else 0)
                        + (1 if y > 0 and orig[p - width] >= 10 else 0)
                        + (1 if y + 1 < rows and orig[p + width] >= 10 else 0)
                    )
                    if dark >= 3:
                        vals[p] = 0
    out = bytearray(len(vals) >> 1)
    mv = memoryview(vals)
    for j, i in enumerate(range(0, len(vals), 2)):
        out[j] = (mv[i] << 4) | mv[i + 1]
    return bytes(out)


def pack_bitmap_2b(buf, width: int, rows: int, pitch: int, lut2: bytes) -> bytes:
    """Pack 8-bit FreeType bitmap into contiguous MSB-first 2bpp: (width * rows + 3) // 4 bytes."""
    total = width * rows
    if total == 0:
        return b""
    abs_pitch = abs(pitch)
    rem = total & 3
    vals = bytearray(total + ((4 - rem) if rem else 0))
    idx = 0
    for y in range(rows):
        row_off = y * abs_pitch if pitch >= 0 else (rows - 1 - y) * abs_pitch
        for x in range(width):
            vals[idx] = lut2[buf[row_off + x]]
            idx += 1
    out = bytearray(len(vals) >> 2)
    mv = memoryview(vals)
    for j, i in enumerate(range(0, len(vals), 4)):
        out[j] = (mv[i] << 6) | (mv[i + 1] << 4) | (mv[i + 2] << 2) | mv[i + 3]
    return bytes(out)


def _pwl_interp(val: int, anchors: list[tuple[float, float]]) -> int:
    if not anchors:
        return val
    if val <= anchors[0][0]:
        return int(round(val + (anchors[0][1] - anchors[0][0])))
    if val >= anchors[-1][0]:
        return int(round(val + (anchors[-1][1] - anchors[-1][0])))
    for i in range(len(anchors) - 1):
        x0, y0 = anchors[i]
        x1, y1 = anchors[i + 1]
        if x0 <= val <= x1:
            if x1 <= x0 + 1e-6:
                return int(round(0.5 * (y0 + y1)))
            return int(round(y0 + ((val - x0) / (x1 - x0)) * (y1 - y0)))
    return val


MAX_STEM_ANCHOR_CONTOURS = 12
MAX_STEM_ANCHOR_POINTS = 200
MAX_CHAIN_SEARCH_STEPS = 512
MIN_COUNTER_GAP_26_6 = 96.0   # 1.5 px minimum inter-leg white counter floor in 26.6 units
MIN_COUNTER_GAP_RATIO = 0.75  # Never narrow an inter-leg counter below 75% of original gap


def _median_float(vals: list[int | float]) -> float:
    s = sorted(vals)
    mid = len(s) >> 1
    if len(s) & 1:
        return float(s[mid])
    return 0.5 * (s[mid - 1] + s[mid])


def _is_italic_face(face) -> bool:
    """Return True if FreeType face is Italic or Oblique (via style_flags or SFNT post.italicAngle)."""
    cached = getattr(face, "_cpfont_is_italic", None)
    if cached is not None:
        return cached

    import freetype

    italic_flag = freetype.FT_STYLE_FLAGS.get("FT_STYLE_FLAG_ITALIC", getattr(freetype, "FT_STYLE_FLAG_ITALIC", 1))
    is_ital = bool(getattr(face, "style_flags", 0) & italic_flag) or abs(getattr(face, "italic_angle", 0)) > 0
    if not is_ital and hasattr(face, "_FT_Face"):
        tag_post = 0x706F7374  # 'post'
        buf = (ctypes.c_ubyte * 16)()
        length = freetype.FT_ULong(16)
        try:
            err = freetype.raw.FT_Load_Sfnt_Table(face._FT_Face, tag_post, 0, buf, ctypes.byref(length))
            if err == 0 and length.value >= 8:
                italic_fixed = struct.unpack(">i", bytes(buf[4:8]))[0]
                if italic_fixed != 0:
                    is_ital = True
        except Exception:
            pass

    try:
        setattr(face, "_cpfont_is_italic", is_ital)
    except Exception:
        pass
    return is_ital


def _is_bicameral_stem_anchor_cp(cp: int) -> bool:
    """Return True for bicameral Latin/Greek/Cyrillic/IPA/ligature codepoints suited to x/cap-height zones."""
    return (
        (0x0020 <= cp <= 0x058F)
        or (0x10A0 <= cp <= 0x10FF)
        or (0x1E00 <= cp <= 0x218F)
        or (0x2C60 <= cp <= 0x2C7F)
        or (0x2D00 <= cp <= 0x2D2F)
        or (0xA720 <= cp <= 0xA7FF)
        or (0xAB30 <= cp <= 0xAB6F)
        or (0xFB00 <= cp <= 0xFB06)
    )


def _probe_stem_anchor_zones(face, load_flags: int) -> dict[str, int]:
    def y_bounds(ch: str):
        gi = face.get_char_index(ord(ch))
        if gi == 0:
            return None
        face.load_glyph(gi, load_flags)
        ol = face.glyph.outline
        if ol.n_contours <= 0 or ol.n_points <= 0:
            return None
        ys = [pt[1] for pt in ol.points]
        return min(ys), max(ys)

    cap_f = [b for b in (y_bounds(c) for c in "BEHTZ") if b is not None]
    x_f = [b for b in (y_bounds(c) for c in "wxyz") if b is not None]
    return {
        "base_flat": int(round(_median_float([b for b, _ in cap_f]) / 64.0) * 64) if cap_f else 0,
        "cap_flat": int(round(_median_float([t for _, t in cap_f]) / 64.0) * 64) if cap_f else 0,
        "x_flat": int(round(_median_float([t for _, t in x_f]) / 64.0) * 64) if x_f else 0,
    }


def _min_allowed_counter_gap(orig_gap: float) -> float:
    if orig_gap <= 0.0:
        return orig_gap
    target = max(MIN_COUNTER_GAP_26_6, MIN_COUNTER_GAP_RATIO * orig_gap)
    return min(orig_gap, target)


def _counter_gaps_ok(stems: list, ds: list[int | float]) -> bool:
    for k in range(1, len(stems)):
        orig_gap = stems[k][0] - stems[k - 1][1]
        new_gap = (stems[k][0] + round(ds[k])) - (stems[k - 1][1] + round(ds[k - 1]))
        if new_gap + 1e-6 < _min_allowed_counter_gap(orig_gap):
            return False
    return True


def apply_stem_anchor_pass(face, ppem: float, zones: dict[str, int], snap_x: bool = True) -> None:
    """Apply one-edge integer stem anchoring + multi-stem pitch lock + <44/64px Y overshoot snap."""
    import freetype

    if face.glyph.format != freetype.FT_GLYPH_FORMAT_OUTLINE:
        return
    ol = face.glyph.outline
    if (
        ol.n_contours <= 0
        or ol.n_points <= 0
        or ol.n_contours > MAX_STEM_ANCHOR_CONTOURS
        or ol.n_points > MAX_STEM_ANCHOR_POINTS
    ):
        return

    raw_ol = ol._FT_Outline
    n_pts = ol.n_points
    pts = [(raw_ol.points[i].x, raw_ol.points[i].y) for i in range(n_pts)]
    on_curve = [bool(raw_ol.tags[i] & 1) for i in range(n_pts)]
    contours = [raw_ol.contours[c] for c in range(ol.n_contours)]
    adv_26_6 = int(round(face.glyph.linearHoriAdvance / 1024.0))
    xh_26_6 = zones["x_flat"] if zones["x_flat"] > 0 else int(round(ppem * 0.511 * 64))
    orient = freetype.raw.FT_Outline_Get_Orientation(ctypes.byref(raw_ol))
    left_sgn = -1 if orient == 1 else 1
    top_sgn = -1 if orient == 1 else 1

    new_x = [p[0] for p in pts]
    new_y = [p[1] for p in pts]

    do_snap_x = bool(snap_x) and not _is_italic_face(face)
    if do_snap_x and adv_26_6 > 0:
        vsegs = []
        start = 0
        for end in contours:
            n = end - start + 1
            edges = []
            for k in range(n):
                i0, i1 = start + k, start + ((k + 1) % n)
                x0, y0 = pts[i0]
                x1, y1 = pts[i1]
                dy = y1 - y0
                adx, ady = abs(x1 - x0), abs(dy)
                max_dx = min(10, max(4, int(0.018 * ady)))
                if ady >= 24 and adx <= max_dx:
                    is_on_on = on_curve[i0] and on_curve[i1] and ady >= 48
                    is_cu2qu_inflect = (not on_curve[i0]) and (not on_curve[i1]) and adx <= 3 and ady >= 128
                    edges.append((k, i0, i1, x0, y0, x1, y1, 1 if dy > 0 else -1, is_on_on or is_cu2qu_inflect))

            used_k = set()
            by_k = {e[0]: e for e in edges}
            for e in edges:
                k, i0, i1, x0, y0, x1, y1, sgn, is_seed = e
                if not is_seed or k in used_k:
                    continue
                x_ref = 0.5 * (x0 + x1)
                chain = [e]
                used_k.add(k)
                kp = (k - 1) % n
                while kp in by_k and kp not in used_k:
                    ep = by_k[kp]
                    if ep[7] == sgn and abs(0.5 * (ep[3] + ep[5]) - x_ref) <= 8:
                        chain.insert(0, ep)
                        used_k.add(kp)
                        kp = (kp - 1) % n
                    else:
                        break
                kn = (k + 1) % n
                while kn in by_k and kn not in used_k:
                    en = by_k[kn]
                    if en[7] == sgn and abs(0.5 * (en[3] + en[5]) - x_ref) <= 8:
                        chain.append(en)
                        used_k.add(kn)
                        kn = (kn + 1) % n
                    else:
                        break
                cy0 = min(min(c[4], c[6]) for c in chain)
                cy1 = max(max(c[4], c[6]) for c in chain)
                if cy1 - cy0 >= 64:
                    w_sum = sum(abs(c[6] - c[4]) for c in chain)
                    x_avg = sum(0.5 * (c[3] + c[5]) * abs(c[6] - c[4]) for c in chain) / max(w_sum, 1)
                    idxs = []
                    for c in chain:
                        idxs.extend([c[1], c[2]])
                    vsegs.append((x_avg, cy0, cy1, sgn, idxs))
            start = end + 1

        vsegs.sort(key=lambda s: (s[3], s[0], s[1]))
        merged = []
        for x, y0, y1, sgn, idxs in vsegs:
            if merged and merged[-1][3] == sgn and abs(merged[-1][0] - x) <= 8:
                px, py0, py1, _, pidxs, pspans = merged[-1]
                wp = sum(b - a for a, b in pspans)
                wc = y1 - y0
                merged[-1] = [
                    (px * wp + x * wc) / max(wp + wc, 1),
                    min(py0, y0),
                    max(py1, y1),
                    sgn,
                    pidxs + idxs,
                    pspans + [(y0, y1)],
                ]
            else:
                merged.append([x, y0, y1, sgn, list(idxs), [(y0, y1)]])

        def _spans_overlap(sp_a: list[tuple[float, float]], sp_b: list[tuple[float, float]]) -> float:
            tot = 0.0
            for a0, a1 in sp_a:
                for b0, b1 in sp_b:
                    ov = min(a1, b1) - max(a0, b0)
                    if ov > 0.0:
                        tot += ov
            return tot

        def _spans_len(sp: list[tuple[float, float]]) -> float:
            return sum(b - a for a, b in sp)

        lefts = sorted([m for m in merged if m[3] == left_sgn], key=lambda m: m[0])
        rights = sorted([m for m in merged if m[3] == -left_sgn], key=lambda m: m[0])
        w_min, w_max = max(48.0, 0.045 * ppem * 64.0), 0.250 * ppem * 64.0
        ov_std, ov_tall = 0.30 * xh_26_6, 0.20 * xh_26_6
        raw_stems, used_r = [], set()
        for lx, ly0, ly1, _, l_i, l_sp in lefts:
            l_len = _spans_len(l_sp)
            for ri, (rx, ry0, ry1, _, r_i, r_sp) in enumerate(rights):
                if ri in used_r:
                    continue
                w = rx - lx
                if w < w_min:
                    continue
                if w > w_max:
                    break
                req_ov = ov_tall if max(l_len, _spans_len(r_sp)) >= 0.70 * xh_26_6 else ov_std
                if _spans_overlap(l_sp, r_sp) >= req_ov:
                    if not any(
                        (lx + 6 < ox < rx - 6) and (_spans_overlap(l_sp, o_sp) >= req_ov)
                        for ox, _, _, _, _, o_sp in lefts
                    ):
                        used_r.add(ri)
                        raw_stems.append((lx, rx, l_i, r_i, min(ly0, ry0), max(ly1, ry1)))
                        break

        raw_stems.sort(key=lambda s: s[0])
        stems = []
        for s in raw_stems:
            if not stems or s[0] >= stems[-1][1] + 16:
                stems.append(s)

        if stems:
            l0, r0 = stems[0][0], stems[0][1]
            dl0 = round(l0 / 64.0) * 64.0 - l0
            dr0 = round(r0 / 64.0) * 64.0 - r0
            d0_p = dl0 if abs(dl0) <= abs(dr0) else dr0
            d0_a = dr0 if abs(dl0) <= abs(dr0) else dl0
            snapped = []
            if len(stems) == 1:
                dk_int = int(round(d0_p))
                snapped = [(l0, l0 + dk_int, r0, r0 + dk_int, dk_int, stems[0][2], stems[0][3], stems[0][4], stems[0][5])]
            else:
                gaps = [stems[k][0] - stems[k - 1][0] for k in range(1, len(stems))]
                eq_pitch = len(gaps) >= 2 and (max(gaps) - min(gaps) <= 20.0)
                best_ds = None
                if eq_pitch:
                    ms = (stems[-1][0] - l0) / (len(stems) - 1)
                    s_lo = math.floor(ms / 64.0) * 64.0
                    cands = []
                    for dc in (d0_p, d0_a):
                        if abs(dc) <= 32.0:
                            for stp in (s_lo, s_lo + 64.0):
                                ds = [int(round((l0 + dc + k * stp) - stems[k][0])) for k in range(len(stems))]
                                if max(abs(d) for d in ds) <= 36 and _counter_gaps_ok(stems, ds):
                                    cands.append((max(abs(d) for d in ds), abs(dc), ds))
                    if cands:
                        best_ds = min(cands)[2]
                else:
                    best_cand: tuple[int, int, list[int]] | None = None
                    search_steps = 0
                    search_aborted = False

                    def _search_chain(idx_k: int, dc_val: int, cur_ds: list[int], cur_max: int) -> None:
                        nonlocal best_cand, search_steps, search_aborted
                        search_steps += 1
                        if search_steps > MAX_CHAIN_SEARCH_STEPS:
                            search_aborted = True
                            return
                        if idx_k == len(stems):
                            cand = (cur_max, abs(dc_val), list(cur_ds))
                            if best_cand is None or cand < best_cand:
                                best_cand = cand
                            return
                        lk = stems[idx_k][0]
                        rp = (lk - l0) / 64.0
                        pn = round(rp) * 64.0
                        pa = math.floor(rp) * 64.0 if pn > rp * 64.0 else math.ceil(rp) * 64.0
                        step_opts: list[float] = []
                        for p_cand in (pn, pa, pn + 64.0):
                            if p_cand not in step_opts:
                                step_opts.append(p_cand)
                        orig_gap = lk - stems[idx_k - 1][1]
                        req_gap = _min_allowed_counter_gap(orig_gap)
                        prev_r_new = stems[idx_k - 1][1] + cur_ds[-1]
                        for p_cand in step_opts:
                            dk_c = int(round((l0 + dc_val + p_cand) - lk))
                            limit = 36 if p_cand > rp * 64.0 else 32
                            if abs(dk_c) <= limit and (lk + dk_c) - prev_r_new + 1e-6 >= req_gap:
                                cur_ds.append(dk_c)
                                _search_chain(idx_k + 1, dc_val, cur_ds, max(cur_max, abs(dk_c)))
                                cur_ds.pop()
                                if search_aborted:
                                    return

                    for dc in (d0_p, d0_a):
                        if abs(dc) > 32.0:
                            continue
                        dc_int = int(round(dc))
                        _search_chain(1, dc_int, [dc_int], abs(dc_int))
                        if search_aborted:
                            break
                    if best_cand is not None and not search_aborted:
                        best_ds = best_cand[2]

                for k, (lk, rk, l_i, r_i, sy0, sy1) in enumerate(stems):
                    if best_ds is not None:
                        dk_int = int(round(best_ds[k]))
                    else:
                        dlk, drk = round(lk / 64.0) * 64.0 - lk, round(rk / 64.0) * 64.0 - rk
                        dk_p = int(round(dlk if abs(dlk) <= abs(drk) else drk))
                        dk_a = int(round(drk if abs(dlk) <= abs(drk) else dlk))
                        if not snapped:
                            dk_int = dk_p
                        else:
                            orig_gap = lk - stems[k - 1][1]
                            req_gap = _min_allowed_counter_gap(orig_gap)
                            prev_r_new = snapped[-1][3]
                            if (lk + dk_p) - prev_r_new + 1e-6 >= req_gap:
                                dk_int = dk_p
                            elif abs(dk_a) <= 36 and (lk + dk_a) - prev_r_new + 1e-6 >= req_gap:
                                dk_int = dk_a
                            else:
                                # Rigidly co-shift with previous stem to preserve 100% of original counter gap
                                dk_int = int(snapped[-1][4])
                    snapped.append((lk, lk + dk_int, rk, rk + dk_int, dk_int, l_i, r_i, sy0, sy1))

            touched_x, touched_c = {}, set()
            pt_c = [0] * n_pts
            cs = 0
            for ci, ce in enumerate(contours):
                for idx in range(cs, ce + 1):
                    pt_c[idx] = ci
                cs = ce + 1
            for lk, lk_n, rk, rk_n, dk_int, l_i, r_i, _, _ in snapped:
                for idx in l_i + r_i:
                    touched_x[idx] = pts[idx][0] + dk_int
                    touched_c.add(pt_c[idx])

            x_anc = []
            if snapped[0][0] > 4.0 and snapped[0][1] > 2.0:
                x_anc.append((0.0, 0.0))
            for lk, lk_n, rk, rk_n, _, _, _, _, _ in snapped:
                x_anc.extend([(lk, lk_n), (rk, rk_n)])
            if snapped[-1][2] < adv_26_6 - 4.0 and snapped[-1][3] < adv_26_6 - 2.0:
                x_anc.append((float(adv_26_6), float(adv_26_6)))

            cs = 0
            for ci, ce in enumerate(contours):
                c_rng = range(cs, ce + 1)
                if ci not in touched_c:
                    c_xs = [pts[idx][0] for idx in c_rng]
                    c_ys = [pts[idx][1] for idx in c_rng]
                    c_mid = 0.5 * (min(c_xs) + max(c_xs))
                    near = min(snapped, key=lambda s: abs(c_mid - 0.5 * (s[0] + s[2])))
                    vert_disjoint = min(c_ys) >= near[8] - 32.0 or max(c_ys) <= near[7] + 32.0
                    if (
                        vert_disjoint
                        and (max(c_xs) - min(c_xs)) <= 0.55 * adv_26_6
                        and abs(c_mid - 0.5 * (near[0] + near[2])) <= 0.28 * ppem * 64.0
                    ):
                        dk_d = int(near[4])
                        for idx in c_rng:
                            new_x[idx] = pts[idx][0] + dk_d
                        cs = ce + 1
                        continue
                for idx in c_rng:
                    if idx in touched_x:
                        new_x[idx] = touched_x[idx]
                    elif pts[idx][0] <= 0 and x_anc[0][0] == 0.0:
                        new_x[idx] = pts[idx][0]
                    elif pts[idx][0] >= adv_26_6 and x_anc[-1][0] == float(adv_26_6):
                        new_x[idx] = pts[idx][0]
                    else:
                        new_x[idx] = _pwl_interp(pts[idx][0], x_anc)
                cs = ce + 1

    # Y-zone <44/64px overshoot collapse + one-edge crossbar snap
    bf, xf, cf = zones["base_flat"], zones["x_flat"], zones["cap_flat"]
    hsegs = []
    start = 0
    min_hl = max(96, int(0.14 * ppem * 64))
    for end in contours:
        n = end - start + 1
        for k in range(n):
            i0, i1 = start + k, start + ((k + 1) % n)
            if on_curve[i0] and on_curve[i1]:
                x0, y0 = pts[i0]
                x1, y1 = pts[i1]
                if abs(y1 - y0) <= 4 and abs(x1 - x0) >= min_hl:
                    hsegs.append((0.5 * (y0 + y1), min(x0, x1), max(x0, x1), 1 if x1 > x0 else -1, [i0, i1]))
        start = end + 1

    hsegs.sort(key=lambda s: (s[3], s[0], s[1]))
    hm = []
    for y, x0, x1, sgn, idxs in hsegs:
        if hm and hm[-1][3] == sgn and abs(hm[-1][0] - y) <= 6:
            py, px0, px1, _, pidxs = hm[-1]
            hm[-1] = [0.5 * (py + y), min(px0, x0), max(px1, x1), sgn, pidxs + idxs]
        else:
            hm.append([y, x0, x1, sgn, list(idxs)])

    bots = sorted([m for m in hm if m[3] == -top_sgn], key=lambda m: m[0])
    tops = sorted([m for m in hm if m[3] == top_sgn], key=lambda m: m[0])
    crossbars, used_t = [], set()
    for by, bx0, bx1, _, b_i in bots:
        if by <= bf + 48:
            continue
        for ti, (ty, tx0, tx1, _, t_i) in enumerate(tops):
            if ti in used_t:
                continue
            h = ty - by
            if h < 0.035 * ppem * 64.0:
                continue
            if h > 0.160 * ppem * 64.0:
                break
            if cf > 0 and ty >= cf - 24 and by > xf + 32:
                continue
            if min(bx1, tx1) - max(bx0, tx0) >= 0.14 * ppem * 64.0:
                used_t.add(ti)
                db, dt = round(by / 64.0) * 64.0 - by, round(ty / 64.0) * 64.0 - ty
                dy = db if abs(db) <= abs(dt) else dt
                if xf > 0 and (ty + dy) > 0.665 * xf and len(contours) >= 2:
                    cs_k = 0
                    for ce_k in contours:
                        if any(cs_k <= idx <= ce_k for idx in t_i):
                            c_ys = [pts[idx][1] for idx in range(cs_k, ce_k + 1)]
                            if min(c_ys) >= ty - 6 and max(c_ys) <= xf + 32:
                                dy -= 64.0
                                break
                        cs_k = ce_k + 1
                crossbars.append((by, by + dy, ty, ty + dy, dy, b_i, t_i))
                break

    bot_ys, x_ys, cap_ys = [], [], []
    start = 0
    for end in contours:
        n = end - start + 1
        if n >= 3:
            ys = [pts[start + k][1] for k in range(n)]
            for k in range(n):
                yk = ys[k]
                p, steps = (k - 1) % n, 0
                while steps < n and ys[p] == yk:
                    p, steps = (p - 1) % n, steps + 1
                if steps == n:
                    continue
                q, steps = (k + 1) % n, 0
                while steps < n and ys[q] == yk:
                    q, steps = (q + 1) % n, steps + 1
                if yk < ys[p] and yk < ys[q] and (bf - 44 < yk < bf):
                    bot_ys.append(yk)
                elif yk > ys[p] and yk > ys[q]:
                    if xf > 0 and (xf < yk < xf + 44):
                        x_ys.append(yk)
                    elif cf > xf + 64 and (cf < yk < cf + 44):
                        cap_ys.append(yk)
        start = end + 1

    if crossbars or bot_ys or x_ys or cap_ys:
        ys_all = [p[1] for p in pts]
        ymin_g, ymax_g = min(ys_all), max(ys_all)
        raw_ya = []
        if ymin_g < bf - 44:
            raw_ya.append((float(ymin_g), float(ymin_g)))
        if bot_ys:
            raw_ya.append((float(min(bot_ys)), float(bf)))
            if any(p[1] == bf for p in pts):
                raw_ya.append((float(bf), float(bf)))
        else:
            raw_ya.append((float(bf), float(bf)))
        for by, by_n, ty, ty_n, _, _, _ in sorted(crossbars, key=lambda c: c[0]):
            raw_ya.extend([(by, by_n), (ty, ty_n)])
        if xf > 0:
            raw_ya.append((float(max(x_ys)), float(xf)) if x_ys else (float(xf), float(xf)))
        if cf > xf + 64:
            if cap_ys:
                raw_ya.append((float(max(cap_ys)), float(cf)))
            elif ymax_g >= cf - 8:
                raw_ya.append((float(cf), float(cf)))
        if ymax_g > max(cf, xf) + 44:
            raw_ya.append((float(ymax_g), float(ymax_g)))

        raw_ya.sort(key=lambda a: (a[0], a[1]))
        y_anc = []
        for oy, ny in raw_ya:
            if not y_anc or (oy >= y_anc[-1][0] and ny >= y_anc[-1][1] and (oy, ny) != y_anc[-1]):
                y_anc.append((oy, ny))

        touched_y = {}
        for _, _, _, _, dy, b_i, t_i in crossbars:
            for idx in b_i + t_i:
                touched_y[idx] = int(round(pts[idx][1] + dy))
        for i in range(n_pts):
            new_y[i] = touched_y[i] if i in touched_y else _pwl_interp(pts[i][1], y_anc)

    for i in range(n_pts):
        raw_ol.points[i].x = int(new_x[i])
        raw_ol.points[i].y = int(new_y[i])


def _pack_loaded_glyph(slot, data_offset, code_point, quant_lut=None, four_bit=False, halo_clip=False):
    """Convert FreeType's rendered glyph slot to (GlyphProps, packed 2-bit or 4-bit bitmap)."""
    if quant_lut is None:
        quant_lut = build_quant_lut(4 if four_bit else 2, 1.0, halo_clip=halo_clip)
    bitmap = slot.bitmap

    # FreeType returns bitmap.pitch as the signed row stride in bytes
    # (negative when stored bottom-up). Cache bitmap.buffer in a local
    # so ctypes does not allocate a wrapper object per pixel, then pack
    # bit-contiguous MSB-first 2bpp or MSB-nibble-first 4bpp.
    buf = bitmap.buffer
    if four_bit:
        packed = pack_bitmap_4b(buf, bitmap.width, bitmap.rows, bitmap.pitch, quant_lut,
                                counter_guard=halo_clip)
    else:
        packed = pack_bitmap_2b(buf, bitmap.width, bitmap.rows, bitmap.pitch, quant_lut)
    glyph = GlyphProps(
        width=bitmap.width,
        height=bitmap.rows,
        advance_x=fp4_from_ft16_16(slot.linearHoriAdvance),
        left=slot.bitmap_left,
        top=slot.bitmap_top,
        data_length=len(packed),
        data_offset=data_offset,
        code_point=code_point,
    )
    return glyph, packed


def rasterize_shaping_glyphs(fontfile, size, load_flags, first_offset, scripts,
                             quant_lut=None, four_bit=False, halo_clip=False):
    """Rasterize every glyph a complex-script shaper can emit for this font
    when shaping the named scripts.

    Returns (glyph entries for codepoints GLYPH_TOKEN_BASE + glyph ID, glyph
    count, packed shaping section).
    """
    import freetype
    import tempfile

    if quant_lut is None:
        quant_lut = build_quant_lut(4 if four_bit else 2, 1.0, halo_clip=halo_clip)

    render_bytes, layout_bytes, glyph_count = shaping_blob.build(fontfile, scripts)
    with tempfile.NamedTemporaryFile(suffix=".ttf", delete=False) as tmp:
        tmp.write(render_bytes)
    try:
        face = freetype.Face(tmp.name)
        face.set_char_size(size << 6, size << 6, 150, 150)
        glyphs = []
        offset = first_offset
        for gid in range(glyph_count):
            face.load_glyph(gid, load_flags)
            glyph, packed = _pack_loaded_glyph(
                face.glyph, offset, shaping_blob.GLYPH_TOKEN_BASE + gid,
                quant_lut=quant_lut, four_bit=four_bit, halo_clip=halo_clip,
            )
            offset += len(packed)
            glyphs.append((glyph, packed))
    finally:
        os.unlink(tmp.name)
    # Same ppem the glyphs were rendered at (and TtfEpdFont uses): size pt at 150 DPI, 26.6.
    ppem26_6 = (size * 150 * 64 + 36) // 72
    return glyphs, glyph_count, shaping_blob.pack_section(ppem26_6, layout_bytes)


def rasterize_font_style(fontfile, size, intervals, style_id=0, force_autohint=False,
                         fallback_fontfile=None, four_bit=False, gamma=1.0,
                         stem_anchor=False, halo_clip=False, shaping=True):
    """Rasterize all glyphs for one font style. Returns StyleRasterData."""
    import freetype

    style_names = {0: "regular", 1: "bold", 2: "italic", 3: "bolditalic"}
    style_label = style_names.get(style_id, str(style_id))

    face = freetype.Face(fontfile)
    # Set font size at 150 DPI (matching fontconvert.py) BEFORE any glyph load.
    # load_glyph() with FT_LOAD_RENDER renders at the active size, so calling
    # it before set_char_size() would waste work at the default size and risk
    # Invalid_Size_Handle on some fonts.
    face.set_char_size(size << 6, size << 6, 150, 150)
    ligature_glyph_indices = extract_ligature_glyph_indices_fonttools(fontfile)
    fallback_face = None
    if fallback_fontfile:
        fallback_face = freetype.Face(fallback_fontfile)
        fallback_face.set_char_size(size << 6, size << 6, 150, 150)

    ppem = size * 150.0 / 72.0
    if stem_anchor:
        outline_flags = freetype.FT_LOAD_DEFAULT | freetype.FT_LOAD_TARGET_LIGHT
        if force_autohint:
            outline_flags |= freetype.FT_LOAD_FORCE_AUTOHINT
        direct_render_flags = outline_flags | freetype.FT_LOAD_RENDER
        primary_zones = _probe_stem_anchor_zones(face, outline_flags)
        fallback_zones = _probe_stem_anchor_zones(fallback_face, outline_flags) if fallback_face else None
        primary_snap_x = style_id in (0, 1) and not _is_italic_face(face)
        fallback_snap_x = (
            style_id in (0, 1)
            and fallback_face is not None
            and not _is_italic_face(fallback_face)
        )
    else:
        load_flags = freetype.FT_LOAD_RENDER
        if force_autohint:
            load_flags |= freetype.FT_LOAD_FORCE_AUTOHINT

    def load_glyph(code_point):
        glyph_index = face.get_char_index(code_point)
        if glyph_index == 0:
            glyph_index = ligature_glyph_indices.get(code_point, 0)
        if glyph_index > 0:
            if stem_anchor:
                if _is_bicameral_stem_anchor_cp(code_point):
                    face.load_glyph(glyph_index, outline_flags)
                    apply_stem_anchor_pass(face, ppem, primary_zones, snap_x=primary_snap_x)
                    face.glyph.render(freetype.FT_RENDER_MODE_NORMAL)
                else:
                    face.load_glyph(glyph_index, direct_render_flags)
            else:
                face.load_glyph(glyph_index, load_flags)
            return face
        if fallback_face:
            fallback_glyph_index = fallback_face.get_char_index(code_point)
            if fallback_glyph_index > 0:
                if stem_anchor:
                    if _is_bicameral_stem_anchor_cp(code_point):
                        fallback_face.load_glyph(fallback_glyph_index, outline_flags)
                        apply_stem_anchor_pass(fallback_face, ppem, fallback_zones, snap_x=fallback_snap_x)
                        fallback_face.glyph.render(freetype.FT_RENDER_MODE_NORMAL)
                    else:
                        fallback_face.load_glyph(fallback_glyph_index, direct_render_flags)
                else:
                    fallback_face.load_glyph(fallback_glyph_index, load_flags)
                return fallback_face
        return None

    # Validate intervals: remove codepoints not present in the font.
    # Only check glyph existence via get_char_index — do NOT call
    # load_glyph here, as that triggers FT_LOAD_RENDER at the target
    # DPI and doubles total rasterization time for no benefit.
    print(f"  [{style_label}] Validating intervals against font...", file=sys.stderr)
    validated_intervals = []
    for i_start, i_end in intervals:
        start = i_start
        for code_point in range(i_start, i_end + 1):
            has_primary = face.get_char_index(code_point) != 0 or code_point in ligature_glyph_indices
            has_fallback = fallback_face and fallback_face.get_char_index(code_point) != 0
            if not has_primary and not has_fallback:
                if start < code_point:
                    validated_intervals.append((start, code_point - 1))
                start = code_point + 1
        if start <= i_end:
            validated_intervals.append((start, i_end))

    intervals = validated_intervals
    total_glyphs = sum(end - start + 1 for start, end in intervals)
    print(f"  [{style_label}] Validated: {len(intervals)} intervals, {total_glyphs} glyphs", file=sys.stderr)

    # Rasterize all glyphs
    total_bitmap_size = 0
    all_glyphs = []
    bpp = 4 if four_bit else 2
    quant_lut = build_quant_lut(bpp, gamma, halo_clip=halo_clip)

    for i_start, i_end in intervals:
        for code_point in range(i_start, i_end + 1):
            f = load_glyph(code_point)
            if f is None:
                glyph = GlyphProps(0, 0, 0, 0, 0, 0, total_bitmap_size, code_point)
                all_glyphs.append((glyph, b''))
                continue

            glyph, packed = _pack_loaded_glyph(
                f.glyph, total_bitmap_size, code_point,
                quant_lut=quant_lut, four_bit=four_bit, halo_clip=halo_clip,
            )
            total_bitmap_size += len(packed)
            all_glyphs.append((glyph, packed))

    # Complex-script shaping: glyphs by glyph ID plus the layout tables.
    shaping_section = b""
    requested = shaping_blob.scripts_in_intervals(intervals) if shaping else []
    scripts = [name for name in requested if shaping_blob.font_supports_script(fontfile, name)]
    if len(scripts) < len(requested):
        unsupported = ", ".join(name for name in requested if name not in scripts)
        print(f"  [{style_label}] Shaping: font has no GSUB for {unsupported}, skipped", file=sys.stderr)
    if scripts:
        shaping_load_flags = direct_render_flags if stem_anchor else load_flags
        shaped_glyphs, shaped_count, shaping_section = rasterize_shaping_glyphs(
            fontfile, size, shaping_load_flags, total_bitmap_size, scripts,
            quant_lut=quant_lut, four_bit=four_bit, halo_clip=halo_clip,
        )
        all_glyphs.extend(shaped_glyphs)
        total_bitmap_size += sum(len(packed) for _, packed in shaped_glyphs)
        base = shaping_blob.GLYPH_TOKEN_BASE
        intervals = intervals + [(base, base + shaped_count - 1)]
        layout_bytes = len(shaping_section) - shaping_blob.SECTION_HEADER_SIZE
        print(f"  [{style_label}] Shaping {', '.join(scripts)}: {shaped_count} glyphs, "
              f"{layout_bytes} bytes of layout tables", file=sys.stderr)
        if layout_bytes > shaping_blob.FLASH_SLOT_BYTES:
            print(f"  [{style_label}] Warning: layout tables exceed {shaping_blob.FLASH_SLOT_BYTES} bytes; "
                  f"boards without PSRAM (X3/X4) keep them in RAM and may not shape", file=sys.stderr)

    # Get font metrics from pipe character (same heuristic as fontconvert.py)
    load_glyph(ord('|'))

    advanceY = norm_ceil(face.size.height)
    ascender = norm_ceil(face.size.ascender)
    descender = norm_floor(face.size.descender)

    print(f"  [{style_label}] Metrics: advanceY={advanceY}, ascender={ascender}, descender={descender}", file=sys.stderr)
    print(f"  [{style_label}] Bitmap: {total_bitmap_size} bytes ({total_bitmap_size / 1024:.1f} KB)", file=sys.stderr)

    # --- Extract kerning and ligatures ---
    ppem = size * 150.0 / 72.0
    all_cps = set(g.code_point for g, _ in all_glyphs)

    kern_map = extract_kerning_fonttools(fontfile, all_cps, ppem)
    # SMP codepoints (> U+FFFF) cannot be stored in the uint16 kern codepoint
    # field; drop them before class derivation to avoid a downstream
    # struct.error when packing the binary kern tables.
    kern_map = {(lcp, rcp): v for (lcp, rcp), v in kern_map.items() if lcp <= 0xFFFF and rcp <= 0xFFFF}
    print(f"  [{style_label}] Kerning: {len(kern_map)} pairs extracted", file=sys.stderr)

    (kern_left_classes, kern_right_classes, kern_matrix,
     kern_left_class_count, kern_right_class_count) = derive_kern_classes(kern_map)

    if kern_map:
        matrix_size = kern_left_class_count * kern_right_class_count
        entries_size = (len(kern_left_classes) + len(kern_right_classes)) * 3
        print(f"  [{style_label}] Kerning classes: {kern_left_class_count} left, {kern_right_class_count} right, "
              f"{matrix_size + entries_size} bytes", file=sys.stderr)

    # SMP codepoints in ligature inputs / outputs are filtered inside
    # extract_ligatures_fonttools (see the codepoints_set filter), so every
    # entry returned here is already 16-bit safe.
    ligature_pairs = extract_ligatures_fonttools(fontfile, all_cps)
    if len(ligature_pairs) > 255:
        print(f"  [{style_label}] WARNING: {len(ligature_pairs)} ligature pairs exceeds uint8_t max (255), truncating",
              file=sys.stderr)
        ligature_pairs = ligature_pairs[:255]
    print(f"  [{style_label}] Ligatures: {len(ligature_pairs)} pairs", file=sys.stderr)

    return StyleRasterData(
        style_id=style_id,
        intervals=intervals,
        all_glyphs=all_glyphs,
        total_bitmap_size=total_bitmap_size,
        advanceY=advanceY,
        ascender=ascender,
        descender=descender,
        kern_left_classes=kern_left_classes,
        kern_right_classes=kern_right_classes,
        kern_matrix=kern_matrix,
        kern_left_class_count=kern_left_class_count,
        kern_right_class_count=kern_right_class_count,
        ligature_pairs=ligature_pairs,
        shaping_section=shaping_section,
    )


# --- Binary packing helpers ---

# EpdGlyph struct: 16 bytes, little-endian
GLYPH_STRUCT_FORMAT = "<BBHhhH2xI"
assert struct.calcsize(GLYPH_STRUCT_FORMAT) == 16


def pack_style_sections(sd):
    """Pack one StyleRasterData into binary section bytearrays.
    Returns (intervals_data, glyphs_data, kern_left, kern_right, kern_matrix, ligatures, bitmaps)."""
    intervals_data = bytearray()
    offset = 0
    for i_start, i_end in sd.intervals:
        intervals_data += struct.pack("<III", i_start, i_end, offset)
        offset += i_end - i_start + 1

    glyphs_data = bytearray()
    for glyph, packed in sd.all_glyphs:
        glyphs_data += struct.pack(GLYPH_STRUCT_FORMAT,
                                   glyph.width, glyph.height, glyph.advance_x,
                                   glyph.left, glyph.top,
                                   glyph.data_length, glyph.data_offset)

    kern_left_data = bytearray()
    for cp, cls in sd.kern_left_classes:
        kern_left_data += struct.pack("<HB", cp, cls)

    kern_right_data = bytearray()
    for cp, cls in sd.kern_right_classes:
        kern_right_data += struct.pack("<HB", cp, cls)

    kern_matrix_data = bytearray()
    if sd.kern_matrix:
        kern_matrix_data = bytearray(struct.pack(f"<{len(sd.kern_matrix)}b", *sd.kern_matrix))

    ligature_data = bytearray()
    for packed_pair, lig_cp in sd.ligature_pairs:
        ligature_data += struct.pack("<II", packed_pair, lig_cp)

    bitmap_data = bytearray()
    for glyph, packed in sd.all_glyphs:
        bitmap_data += packed
    assert len(bitmap_data) == sd.total_bitmap_size

    return (intervals_data, glyphs_data, kern_left_data, kern_right_data,
            kern_matrix_data, ligature_data, bitmap_data)


def style_sections_total_size(sections):
    """Total byte size of all sections returned by pack_style_sections()."""
    return sum(len(s) for s in sections)


# --- File writers ---

def generate_cpfont_multistyle(style_fonts, size, intervals, output_path,
                               force_autohint=False, fallback_style_fonts=None,
                               four_bit=False, gamma=1.0,
                               stem_anchor=False, halo_clip=False, shaping=True):
    """Generate a multi-style v4 .cpfont file.

    style_fonts: dict of {style_id: fontfile_path} e.g. {0: "Regular.ttf", 2: "Italic.ttf"}
    fallback_style_fonts: optional dict of {style_id: fallback_fontfile_path}
    four_bit: when True, emit 4-bit (16-level, flags=0x0002) bitmaps instead of 2-bit (flags=0x0001)
    gamma: reflectance-space quantization gamma (default 1.0)
    stem_anchor: opt-in one-edge integer stem anchoring & <44/64px Y overshoot collapse (default False)
    halo_clip: opt-in <=15/255 outer-halo dead-zone clip & tight-counter guard (default False)
    """
    MAGIC = b"CPFONT\x00\x00"
    HEADER_SIZE = 32
    STYLE_TOC_ENTRY_SIZE = 32
    flags = CPFONT_FLAG_4BIT if four_bit else CPFONT_FLAG_2BIT
    style_count = len(style_fonts)

    # Rasterize each style
    raster_data = {}  # style_id -> StyleRasterData
    fallback_style_fonts = fallback_style_fonts or {}
    for style_id in sorted(style_fonts.keys()):
        fontfile = style_fonts[style_id]
        fallback_fontfile = fallback_style_fonts.get(style_id)
        print(f"  Rasterizing style {style_id}...", file=sys.stderr)
        raster_data[style_id] = rasterize_font_style(
            fontfile, size, intervals, style_id=style_id,
            force_autohint=force_autohint,
            fallback_fontfile=fallback_fontfile,
            four_bit=four_bit,
            gamma=gamma,
            stem_anchor=stem_anchor,
            halo_clip=halo_clip,
            shaping=shaping)

    # Pack binary sections for each style
    packed_sections = {}  # style_id -> tuple of section bytearrays
    for style_id, sd in raster_data.items():
        packed_sections[style_id] = pack_style_sections(sd)

    # Calculate data offsets (after header + TOC)
    data_start = HEADER_SIZE + style_count * STYLE_TOC_ENTRY_SIZE
    current_offset = data_start

    style_offsets = {}  # style_id -> absolute file offset
    for style_id in sorted(packed_sections.keys()):
        style_offsets[style_id] = current_offset
        current_offset += style_sections_total_size(packed_sections[style_id])

    # Shaping sections follow every style's data; readers that predate them
    # never look past the bitmaps, and see zero in the TOC field.
    shaping_offsets = {}  # style_id -> absolute file offset, 0 when none
    for style_id in sorted(raster_data.keys()):
        section = raster_data[style_id].shaping_section
        shaping_offsets[style_id] = current_offset if section else 0
        current_offset += len(section)

    # Build global header
    # V4 header: magic(8) + version(2) + flags(2) + styleCount(1) + reserved(19) = 32
    header = struct.pack("<8sHHB19s", MAGIC, CPFONT_VERSION, flags, style_count, bytes(19))
    assert len(header) == HEADER_SIZE

    # Build style TOC entries
    # Each entry: styleId(1) + pad(3) + intervalCount(4) + glyphCount(4) +
    #   advanceY(1) + ascender(2) + descender(2) + kernL(2) + kernR(2) +
    #   kernLCls(1) + kernRCls(1) + ligCount(1) + dataOffset(4) + shapingOffset(4) = 32
    STYLE_TOC_FORMAT = "<B3xIIBhhHHBBBII"
    assert struct.calcsize(STYLE_TOC_FORMAT) == STYLE_TOC_ENTRY_SIZE

    toc_data = bytearray()
    for style_id in sorted(raster_data.keys()):
        sd = raster_data[style_id]
        if sd.advanceY > 255:
            print(f"ERROR: advanceY ({sd.advanceY}) exceeds uint8 range for "
                  f"style {style_id} size {size}. This likely means the font "
                  f"size is too large for this format.",
                  file=sys.stderr)
            sys.exit(1)
        toc_data += struct.pack(STYLE_TOC_FORMAT,
                                style_id,
                                len(sd.intervals), len(sd.all_glyphs),
                                sd.advanceY, sd.ascender, sd.descender,
                                len(sd.kern_left_classes), len(sd.kern_right_classes),
                                sd.kern_left_class_count, sd.kern_right_class_count,
                                len(sd.ligature_pairs),
                                style_offsets[style_id],
                                shaping_offsets[style_id])

    # Write output
    os.makedirs(os.path.dirname(output_path) if os.path.dirname(output_path) else ".", exist_ok=True)
    total_file_size = 0
    with open(output_path, "wb") as f:
        f.write(header)
        f.write(toc_data)
        for style_id in sorted(packed_sections.keys()):
            for section in packed_sections[style_id]:
                f.write(section)
        for style_id in sorted(raster_data.keys()):
            assert raster_data[style_id].shaping_section == b"" or f.tell() == shaping_offsets[style_id]
            f.write(raster_data[style_id].shaping_section)
        total_file_size = f.tell()

    # Print summary
    depth_label = "4-bit" if four_bit else "2-bit"
    print(f"  Output: {output_path} (v4, {depth_label}, {style_count} styles)", file=sys.stderr)
    print(f"    Header+TOC: {HEADER_SIZE + len(toc_data)} bytes", file=sys.stderr)
    for style_id in sorted(raster_data.keys()):
        sd = raster_data[style_id]
        secs = packed_sections[style_id]
        style_names = {0: "regular", 1: "bold", 2: "italic", 3: "bolditalic"}
        sname = style_names.get(style_id, str(style_id))
        ssize = style_sections_total_size(secs)
        print(f"    {sname}: {len(sd.all_glyphs)} glyphs, {len(sd.intervals)} intervals, "
              f"{ssize} bytes", file=sys.stderr)
    print(f"    Total: {total_file_size} bytes ({total_file_size / 1024 / 1024:.2f} MB)", file=sys.stderr)
    return total_file_size


def main():
    parser = argparse.ArgumentParser(
        description="Generate .cpfont files for SD card font loading.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=f"Available interval presets: {', '.join(sorted(INTERVAL_PRESETS.keys()))}"
    )

    # Font file (positional, optional for multi-style mode)
    parser.add_argument("fontfile", nargs="?", default=None,
                        help="Path to the font file (single-style mode).")
    parser.add_argument("--intervals", dest="intervals",
                        help="Comma-separated interval presets (e.g., 'latin-ext,greek,cyrillic').")
    parser.add_argument("--size", type=int, dest="size",
                        help="Single font size to generate.")
    parser.add_argument("--sizes", dest="sizes",
                        help="Comma-separated sizes (e.g., '12,14,16,18').")
    parser.add_argument("--style", dest="style", default="regular",
                        choices=["regular", "bold", "italic", "bolditalic"],
                        help="Font style for single-style mode (default: regular).")
    parser.add_argument("--name", dest="name",
                        help="Font family name for output filenames (default: derived from font filename).")
    parser.add_argument("--force-autohint", dest="force_autohint", action="store_true",
                        help="Force FreeType auto-hinter instead of native font hinting.")
    depth_group = parser.add_mutually_exclusive_group()
    depth_group.add_argument("-2", "--2bit", dest="two_bit", action="store_true",
                             help="Emit 2-bit (4-level) glyph bitmaps, flags=0x0001 (default).")
    depth_group.add_argument("-4", "--4bit", dest="four_bit", action="store_true",
                             help="Emit 4-bit (16-level, MSB-nibble-first) glyph bitmaps, flags=0x0002.")
    parser.add_argument("--gamma", type=float, default=1.0,
                        help="Reflectance-space quantization gamma (default: 1.0; e.g. 1.10 with build-sd-fonts.py --4bit).")
    parser.add_argument("--stem-anchor", dest="stem_anchor", action="store_true",
                        help="Opt-in one-edge integer stem anchoring, pitch lock, and <44/64px Y overshoot collapse.")
    parser.add_argument("--halo-clip", dest="halo_clip", action="store_true",
                        help="Opt-in <=15/255 outer-halo dead-zone clip (and 4bpp >=240/255 clip + tight-counter guard).")
    parser.add_argument("-o", "--output", dest="output",
                        help="Output file path (for single-size mode).")
    parser.add_argument("--output-dir", dest="output_dir",
                        help="Output directory for multi-size mode.")
    parser.add_argument("--list-presets", action="store_true",
                        help="List available interval presets and exit.")
    parser.add_argument("--no-shaping", dest="shaping", action="store_false",
                        help="Skip the complex-script shaping section even when the intervals include an Indic script.")

    # Multi-style mode: per-style font file arguments (generates v4 .cpfont)
    parser.add_argument("--regular", dest="font_regular",
                        help="Font file for regular style (enables multi-style v4 mode).")
    parser.add_argument("--bold", dest="font_bold",
                        help="Font file for bold style.")
    parser.add_argument("--italic", dest="font_italic",
                        help="Font file for italic style.")
    parser.add_argument("--bolditalic", dest="font_bolditalic",
                        help="Font file for bold-italic style.")
    parser.add_argument("--fallback-regular", dest="fallback_regular",
                        help="Fallback font file for regular style.")
    parser.add_argument("--fallback-bold", dest="fallback_bold",
                        help="Fallback font file for bold style.")
    parser.add_argument("--fallback-italic", dest="fallback_italic",
                        help="Fallback font file for italic style.")
    parser.add_argument("--fallback-bolditalic", dest="fallback_bolditalic",
                        help="Fallback font file for bold-italic style.")

    args = parser.parse_args()

    if args.list_presets:
        print("Available interval presets:")
        for name, ranges in sorted(INTERVAL_PRESETS.items()):
            total = sum(e - s + 1 for s, e in ranges)
            print(f"  {name:15s}  {len(ranges)} range(s), ~{total} codepoints")
        sys.exit(0)

    if args.gamma <= 0.0 or not math.isfinite(args.gamma):
        parser.error("--gamma must be finite and greater than 0")

    # Detect multi-style mode
    style_fonts = {}
    if args.font_regular:
        style_fonts[0] = args.font_regular
    if args.font_bold:
        style_fonts[1] = args.font_bold
    if args.font_italic:
        style_fonts[2] = args.font_italic
    if args.font_bolditalic:
        style_fonts[3] = args.font_bolditalic

    fallback_style_fonts = {}
    if args.fallback_regular:
        fallback_style_fonts[0] = args.fallback_regular
    if args.fallback_bold:
        fallback_style_fonts[1] = args.fallback_bold
    if args.fallback_italic:
        fallback_style_fonts[2] = args.fallback_italic
    if args.fallback_bolditalic:
        fallback_style_fonts[3] = args.fallback_bolditalic

    is_multistyle = len(style_fonts) > 0
    fontfile = args.fontfile

    # Require --intervals
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

    # Validate early: single-style mode requires a font file
    if not is_multistyle and not fontfile:
        print("Error: fontfile is required in single-style mode", file=sys.stderr)
        sys.exit(1)

    # Determine font name
    if args.name:
        font_name = args.name
    elif is_multistyle:
        # Derive from the regular font file
        ref_file = style_fonts[min(style_fonts.keys())]
        base = os.path.splitext(os.path.basename(ref_file))[0]
        for suffix in ["-Regular", "-Bold", "-Italic", "-BoldItalic",
                       "-regular", "-bold", "-italic", "-bolditalic"]:
            if base.endswith(suffix):
                base = base[:-len(suffix)]
                break
        font_name = base
    else:
        base = os.path.splitext(os.path.basename(fontfile))[0]
        for suffix in ["-Regular", "-Bold", "-Italic", "-BoldItalic",
                       "-regular", "-bold", "-italic", "-bolditalic"]:
            if base.endswith(suffix):
                base = base[:-len(suffix)]
                break
        font_name = base

    if not is_multistyle:
        # Single font file provided: wrap as a single-style v4 font
        style_map = {"regular": 0, "bold": 1, "italic": 2, "bolditalic": 3}
        style_fonts[style_map[args.style]] = fontfile

    # Always generate v4 format
    if args.output and len(sizes) != 1:
        print("Error: --output can only be used with a single size", file=sys.stderr)
        sys.exit(1)
    output_dir = args.output_dir if args.output_dir else f"{font_name}/"
    total_size = 0
    for sz in sizes:
        if args.output and len(sizes) == 1:
            output_path = args.output
        else:
            filename = f"{font_name}_{sz}.cpfont"
            output_path = os.path.join(output_dir, filename)
        depth_label = "4-bit" if args.four_bit else "2-bit"
        print(f"Generating {output_path} (size {sz}, {len(style_fonts)} style(s), v4, {depth_label})...",
              file=sys.stderr)
        total_size += generate_cpfont_multistyle(
            style_fonts, sz, intervals, output_path,
            force_autohint=args.force_autohint,
            fallback_style_fonts=fallback_style_fonts,
            four_bit=args.four_bit,
            gamma=args.gamma,
            stem_anchor=args.stem_anchor,
            halo_clip=args.halo_clip,
            shaping=args.shaping)
    print(f"\nTotal: {len(sizes)} files, {total_size / 1024 / 1024:.2f} MB", file=sys.stderr)


if __name__ == "__main__":
    main()
