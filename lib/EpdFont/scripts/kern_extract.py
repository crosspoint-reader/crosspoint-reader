"""Read a font's pair kerning from its layout tables.

FreeType's get_kerning() reads only the legacy 'kern' table, while modern fonts keep their kerning
in the GPOS 'kern' feature. extract_raw_kerning() reads both: the legacy table, and the feature's
PairPos lookups (type 2, and type 9 Extension lookups wrapping them) in both formats.

Imported by fontconvert_sdcard.py and fontconvert.py, which used to carry a copy each.
"""

import sys


def _pairpos_subtable(subtable, glyphs, raw_kern):
    """Adds what a PairPos subtable (format 1 or 2) kerns among `glyphs` to raw_kern."""
    if subtable.Format == 1:
        # Individual pairs
        for i, coverage_glyph in enumerate(subtable.Coverage.glyphs):
            if coverage_glyph not in glyphs:
                continue
            pair_set = subtable.PairSet[i]
            for pvr in pair_set.PairValueRecord:
                if pvr.SecondGlyph not in glyphs:
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
        left_by_class = {}   # only glyphs in coverage AND requested
        for glyph in glyphs:
            if glyph not in coverage_set:
                continue
            c1 = class_def1.get(glyph, 0)
            left_by_class.setdefault(c1, []).append(glyph)

        right_by_class = {}  # all requested glyphs
        for glyph in glyphs:
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


def extract_raw_kerning(font, glyphs):
    """{(left glyph name, right glyph name): adjustment in design units} for the pairs of `glyphs`
    (glyph names: a set or a dict's keys) that `font` (a fontTools TTFont) kerns."""
    raw_kern = {}

    # 1. Legacy kern table
    if 'kern' in font:
        for subtable in font['kern'].kernTables:
            if hasattr(subtable, 'kernTable'):
                for (lg, rg), val in subtable.kernTable.items():
                    if lg in glyphs and rg in glyphs:
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
                # `lookup.LookupType` is still 9 and the unwrapped subtable
                # carries `Format` rather than `LookupType`, so the *effective*
                # type for the dispatch below comes from `st.ExtensionLookupType`.
                if lookup.LookupType == 9 and hasattr(st, 'ExtSubTable'):
                    actual = st.ExtSubTable
                effective_type = getattr(st, 'ExtensionLookupType', lookup.LookupType)
                if hasattr(actual, 'Format'):
                    # Only PairPos (type 2) is kerning: another type under 'kern' (contextual
                    # positioning, say) has a Format too, but none of the fields read above.
                    if effective_type == 2:
                        _pairpos_subtable(actual, glyphs, raw_kern)
                    else:
                        print(f"  Debug: skipping unsupported GPOS kern lookupType="
                              f"{effective_type} (outer={lookup.LookupType}, Format={actual.Format})",
                              file=sys.stderr)
    return raw_kern
