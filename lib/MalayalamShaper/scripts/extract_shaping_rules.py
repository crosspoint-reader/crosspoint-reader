#!/usr/bin/env python3
"""
Extract Malayalam shaping rules from a font's GSUB table.

Produces two outputs:
1. A C++ header (MalayalamShapingData.h) with lookup tables for the firmware shaper.
2. A glyph-name-to-PUA mapping file used by fontconvert.py to include private glyphs.

Usage:
    python extract_shaping_rules.py <font.otf> <output_dir>

Example:
    python extract_shaping_rules.py ../../EpdFont/builtinFonts/source/Manjari/Manjari-Regular.otf ..
"""

import sys
import os
import json
from fontTools.ttLib import TTFont

PUA_START = 0xE000  # Start of Private Use Area

# OpenType features relevant to Malayalam shaping, in application order
MALAYALAM_FEATURES = ['akhn', 'half', 'blwf', 'pref', 'pres', 'pstf', 'psts', 'blws']


def extract_ligatures_from_lookup(table, lookup_idx):
    """Extract ligature rules from a single Type 4 lookup."""
    lookup = table.LookupList.Lookup[lookup_idx]
    results = []
    if lookup.LookupType != 4:
        return results
    for subtable in lookup.SubTable:
        for first_glyph, ligatures in subtable.ligatures.items():
            for lig in ligatures:
                components = [first_glyph] + lig.Component
                output = lig.LigGlyph
                results.append((components, output))
    return results


def collect_chained_lookups(table, lookup_idx):
    """Follow Type 6 (ChainingContext) lookups and return referenced lookup indices."""
    lookup = table.LookupList.Lookup[lookup_idx]
    if lookup.LookupType != 6:
        return set()
    referenced = set()
    for subtable in lookup.SubTable:
        if hasattr(subtable, 'SubstLookupRecord'):
            for rec in subtable.SubstLookupRecord:
                referenced.add(rec.LookupListIndex)
    return referenced


def extract_ligature_rules(font_path):
    """Extract all ligature substitution rules from Malayalam GSUB features.

    Handles Type 4 (LigatureSubst) directly and follows Type 6
    (ChainingContext) references to reach indirectly-referenced Type 4
    lookups.
    """
    font = TTFont(font_path)
    gsub = font['GSUB']
    table = gsub.table
    cmap = font.getBestCmap()
    rev_cmap = {v: k for k, v in cmap.items()}

    rules_by_feature = {}
    seen_lookups = set()

    for rec in table.FeatureList.FeatureRecord:
        if rec.FeatureTag not in MALAYALAM_FEATURES:
            continue
        tag = rec.FeatureTag

        # Collect all lookup indices: direct + indirectly referenced via Type 6
        all_lookup_indices = set()
        for lookup_idx in rec.Feature.LookupListIndex:
            all_lookup_indices.add(lookup_idx)
            # Follow Type 6 chains
            chained = collect_chained_lookups(table, lookup_idx)
            all_lookup_indices.update(chained)

        for lookup_idx in sorted(all_lookup_indices):
            if lookup_idx in seen_lookups:
                continue
            seen_lookups.add(lookup_idx)
            rules = extract_ligatures_from_lookup(table, lookup_idx)
            for components, output in rules:
                rules_by_feature.setdefault(tag, []).append((components, output))

    return font, cmap, rev_cmap, rules_by_feature


def assign_pua_codepoints(rules_by_feature, rev_cmap):
    """Assign PUA codepoints to private glyphs that lack Unicode mappings."""
    # Collect all unique output glyph names
    all_output_glyphs = set()
    for rules in rules_by_feature.values():
        for _, output in rules:
            all_output_glyphs.add(output)

    # Also collect intermediate glyphs that are inputs to later rules
    all_input_glyphs = set()
    for rules in rules_by_feature.values():
        for components, _ in rules:
            for g in components:
                all_input_glyphs.add(g)

    # Private glyphs = those without Unicode codepoints that appear as outputs
    private_glyphs = sorted(g for g in all_output_glyphs if g not in rev_cmap)

    # Assign PUA codepoints
    pua_map = {}  # glyph_name -> PUA codepoint
    for i, gname in enumerate(private_glyphs):
        pua_map[gname] = PUA_START + i

    if PUA_START + len(private_glyphs) > 0xF8FF:
        print(f"ERROR: Need {len(private_glyphs)} PUA slots but only {0xF8FF - PUA_START + 1} available",
              file=sys.stderr)
        sys.exit(1)

    return pua_map


def build_shaping_table(rules_by_feature, rev_cmap, pua_map):
    """Build the shaping rules as (input_codepoints, output_codepoint) tuples."""
    shaping_rules = []

    def glyph_to_cp(gname):
        """Convert glyph name to codepoint (Unicode or PUA)."""
        if gname in rev_cmap:
            return rev_cmap[gname]
        if gname in pua_map:
            return pua_map[gname]
        return None

    for feature in MALAYALAM_FEATURES:
        rules = rules_by_feature.get(feature, [])
        for components, output in rules:
            input_cps = [glyph_to_cp(g) for g in components]
            output_cp = glyph_to_cp(output)
            if all(cp is not None for cp in input_cps) and output_cp is not None:
                shaping_rules.append((input_cps, output_cp, feature))

    return shaping_rules


def generate_cpp_header(shaping_rules, pua_map, output_path):
    """Generate C++ header with static constexpr shaping tables."""
    # Sort rules by first input codepoint for efficient lookup
    shaping_rules.sort(key=lambda r: (r[0][0], len(r[0]), r[0]))

    # Group rules by input length for separate tables
    rules_by_len = {}
    for input_cps, output_cp, feature in shaping_rules:
        length = len(input_cps)
        rules_by_len.setdefault(length, []).append((input_cps, output_cp))

    with open(output_path, 'w') as f:
        f.write("// Generated by extract_shaping_rules.py - DO NOT EDIT\n")
        f.write("// Source font: Manjari (Swathanthra Malayalam Computing)\n")
        f.write("#pragma once\n\n")
        f.write("#include <cstdint>\n\n")
        f.write("namespace MalayalamShapingData {\n\n")

        # PUA range info
        f.write(f"static constexpr uint32_t PUA_START = 0x{PUA_START:04X};\n")
        f.write(f"static constexpr uint32_t PUA_END = 0x{PUA_START + len(pua_map) - 1:04X};\n")
        f.write(f"static constexpr uint16_t PUA_GLYPH_COUNT = {len(pua_map)};\n\n")

        # Generate tables for each input length
        for length in sorted(rules_by_len.keys()):
            rules = rules_by_len[length]
            # Sort for binary search
            rules.sort(key=lambda r: r[0])

            f.write(f"// Rules with {length} input codepoints ({len(rules)} entries)\n")
            f.write(f"struct Rule{length} {{\n")
            for i in range(length):
                f.write(f"  uint32_t in{i};\n")
            f.write(f"  uint32_t out;\n")
            f.write(f"}};\n\n")

            f.write(f"static constexpr Rule{length} rules{length}[] = {{\n")
            for input_cps, output_cp in rules:
                in_str = ", ".join(f"0x{cp:04X}" for cp in input_cps)
                f.write(f"  {{{in_str}, 0x{output_cp:04X}}},\n")
            f.write(f"}};\n")
            f.write(f"static constexpr uint16_t rules{length}Count = {len(rules)};\n\n")

        f.write("}  // namespace MalayalamShapingData\n")

    print(f"Generated {output_path}")
    print(f"  Rules by input length: {', '.join(f'{k}-char: {len(v)}' for k, v in sorted(rules_by_len.items()))}")


def generate_pua_mapping(pua_map, font, output_path):
    """Generate JSON mapping of PUA codepoints to glyph names for fontconvert.py."""
    # Save as JSON for fontconvert.py to consume
    mapping = {f"0x{cp:04X}": gname for gname, cp in sorted(pua_map.items(), key=lambda x: x[1])}

    with open(output_path, 'w') as f:
        json.dump(mapping, f, indent=2)

    print(f"Generated {output_path}")
    print(f"  {len(mapping)} PUA glyph mappings (U+{PUA_START:04X} - U+{PUA_START + len(mapping) - 1:04X})")


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <font.otf> <output_dir>", file=sys.stderr)
        sys.exit(1)

    font_path = sys.argv[1]
    output_dir = sys.argv[2]

    print(f"Extracting shaping rules from: {font_path}")
    font, cmap, rev_cmap, rules_by_feature = extract_ligature_rules(font_path)

    total_rules = sum(len(r) for r in rules_by_feature.values())
    print(f"  Total ligature rules: {total_rules}")
    for feat, rules in sorted(rules_by_feature.items()):
        print(f"    {feat}: {len(rules)} rules")

    print("\nAssigning PUA codepoints to private glyphs...")
    pua_map = assign_pua_codepoints(rules_by_feature, rev_cmap)
    print(f"  Assigned {len(pua_map)} PUA codepoints")

    print("\nBuilding shaping table...")
    shaping_rules = build_shaping_table(rules_by_feature, rev_cmap, pua_map)
    print(f"  {len(shaping_rules)} total shaping rules")

    # Generate outputs
    header_path = os.path.join(output_dir, "MalayalamShapingData.h")
    generate_cpp_header(shaping_rules, pua_map, header_path)

    mapping_path = os.path.join(output_dir, "scripts", "pua_mapping.json")
    generate_pua_mapping(pua_map, font, mapping_path)


if __name__ == "__main__":
    main()
