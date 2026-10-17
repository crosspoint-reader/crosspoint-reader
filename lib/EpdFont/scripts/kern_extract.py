"""Read a font's pair kerning from its layout tables, the way an OpenType shaper applies it.

FreeType's get_kerning() reads only the legacy 'kern' table, while modern fonts keep their kerning
in the GPOS 'kern' feature. extract_raw_kerning() reads both: the legacy table, and the feature's
PairPos lookups (type 2, and type 9 Extension lookups wrapping them) in both formats, by the rules
a shaper follows:
  - the 'kern' feature's lookups each add their adjustment;
  - within one lookup, the FIRST subtable that matches a pair decides it, even with a zero value.
    A PairPos format 1 subtable matches only the pairs it lists; a format 2 subtable matches every
    pair whose left glyph it covers, through its class matrix (class 0 included). Fonts put their
    pair exceptions (format 1) ahead of their class kerning (format 2) for exactly this;
  - the legacy 'kern' table only counts when GPOS has no 'kern' feature. Fonts that carry both
    repeat their pairs in each.
Adding up every subtable and both tables instead kerned exception pairs twice: Literata's "Va" came
out at -160 units instead of -70, Inter's "L*" at -348 instead of -81.

Imported by fontconvert_sdcard.py and fontconvert.py, which used to carry a copy each. The browser
converter reads kerning the same way in tools/ftwasm/js (cpfont-writer.mjs prepareKerning(),
otf-tables.mjs pairPos()); test_kern_extract.py checks both on the same fonts.

Based on crosspoint-reader PRs #3795 and #3800 (Stefan VanBuren, @stefanvanburen).
"""

import sys


def _pairpos_subtable(subtable, glyphs, raw_kern, claimed_pairs, claimed_left):
    """Adds what a PairPos subtable (format 1 or 2) kerns among `glyphs` to raw_kern.

    claimed_pairs and claimed_left are what earlier subtables of the same lookup matched: the pairs
    a format 1 subtable listed, and the left glyphs a format 2 subtable covered (all their pairs).
    Those are skipped here, and this subtable's matches are added to them.
    """
    if subtable.Format == 1:
        # Individual pairs: a listed pair matches whatever its value
        for i, coverage_glyph in enumerate(subtable.Coverage.glyphs):
            if coverage_glyph not in glyphs or coverage_glyph in claimed_left:
                continue
            pair_set = subtable.PairSet[i]
            for pvr in pair_set.PairValueRecord:
                key = (coverage_glyph, pvr.SecondGlyph)
                if pvr.SecondGlyph not in glyphs or key in claimed_pairs:
                    continue
                claimed_pairs.add(key)
                xa = 0
                if hasattr(pvr, 'Value1') and pvr.Value1:
                    xa = getattr(pvr.Value1, 'XAdvance', 0) or 0
                if xa != 0:
                    raw_kern[key] = raw_kern.get(key, 0) + xa
    elif subtable.Format == 2:
        # Class-based pairs — iterate by class, not by glyph, to avoid
        # O(glyphs²) explosion for CJK fonts with many requested glyphs.
        class_def1 = subtable.ClassDef1.classDefs if subtable.ClassDef1 else {}
        class_def2 = subtable.ClassDef2.classDefs if subtable.ClassDef2 else {}
        coverage_set = set(subtable.Coverage.glyphs)

        # Build reverse mappings: class_id -> list of glyph names
        left_by_class = {}   # only glyphs in coverage AND requested AND not claimed yet
        for glyph in glyphs:
            if glyph not in coverage_set or glyph in claimed_left:
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
                        if key not in claimed_pairs:
                            raw_kern[key] = raw_kern.get(key, 0) + xa
            # A covered left glyph matches here with every right glyph, zero values included.
            claimed_left.update(left_by_class[c1])


def extract_raw_kerning(font, glyphs):
    """{(left glyph name, right glyph name): adjustment in design units} for the pairs of `glyphs`
    (glyph names: a set or a dict's keys) that `font` (a fontTools TTFont) kerns."""
    raw_kern = {}

    gpos = font['GPOS'].table if 'GPOS' in font else None
    kern_features = [fr for fr in gpos.FeatureList.FeatureRecord
                     if fr.FeatureTag == 'kern'] if gpos and gpos.FeatureList else []

    # 1. Legacy kern table, only when GPOS has no 'kern' feature
    if 'kern' in font and not kern_features:
        for subtable in font['kern'].kernTables:
            if hasattr(subtable, 'kernTable'):
                for (lg, rg), val in subtable.kernTable.items():
                    if lg in glyphs and rg in glyphs:
                        raw_kern[(lg, rg)] = raw_kern.get((lg, rg), 0) + val

    # 2. GPOS 'kern' feature. Lookups add up, so the order they are read in does not matter.
    if kern_features:
        kern_lookup_indices = set()
        for fr in kern_features:
            kern_lookup_indices.update(fr.Feature.LookupListIndex)
        for li in kern_lookup_indices:
            lookup = gpos.LookupList.Lookup[li]
            claimed_pairs, claimed_left = set(), set()
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
                        _pairpos_subtable(actual, glyphs, raw_kern, claimed_pairs, claimed_left)
                    else:
                        print(f"  Debug: skipping unsupported GPOS kern lookupType="
                              f"{effective_type} (outer={lookup.LookupType}, Format={actual.Format})",
                              file=sys.stderr)
    return raw_kern
