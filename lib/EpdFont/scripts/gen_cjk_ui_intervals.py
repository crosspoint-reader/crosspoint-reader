#!/usr/bin/env python3
"""
Compute the minimal set of --additional-intervals for fontconvert.py needed to
render a translation file's non-ASCII characters in a built-in UI font.

Reads one or more I18n translation YAML files, collects every character used
in their string values (optionally restricted to a set of top-level keys),
drops anything already covered by a given base font's cmap, and prints the
remaining codepoints as coalesced (min,max) intervals — one per line — for a
caller to turn into `--additional-intervals min,max` arguments.

Usage:
  # Full translation content (every string in the file):
  python gen_cjk_ui_intervals.py --yaml ../../I18n/translations/chinese-traditional.yaml \
      --exclude-cmap ../builtinFonts/source/Ubuntu/Ubuntu-Regular.ttf

  # Just the language's own name, across every translation file, restricted to
  # CJK codepoint ranges (used to keep every CJK language's entry legible in
  # the language picker without merging that language's full script into the
  # built-in UI fonts, and without dragging in unrelated scripts also present
  # in some other language's name, e.g. Arabic/Hebrew):
  python gen_cjk_ui_intervals.py --yaml ../../I18n/translations/*.yaml \
      --only-keys _language_name --codepoint-range 0x4E00,0x9FFF --codepoint-range 0x3040,0x30FF \
      --exclude-cmap ../builtinFonts/source/Ubuntu/Ubuntu-Regular.ttf
"""
import argparse
import sys

import yaml
from fontTools.ttLib import TTFont


def collect_chars(yaml_paths, only_keys=None):
    chars = set()

    def walk(value):
        if isinstance(value, str):
            chars.update(value)
        elif isinstance(value, dict):
            for v in value.values():
                walk(v)
        elif isinstance(value, list):
            for v in value:
                walk(v)

    for path in yaml_paths:
        with open(path, encoding="utf-8") as f:
            data = yaml.safe_load(f)
        if only_keys:
            for key in only_keys:
                if key in data:
                    walk(data[key])
        else:
            walk(data)
    return chars


def coalesce(codepoints):
    ordered = sorted(codepoints)
    if not ordered:
        return []
    intervals = []
    start = prev = ordered[0]
    for cp in ordered[1:]:
        if cp == prev + 1:
            prev = cp
            continue
        intervals.append((start, prev))
        start = prev = cp
    intervals.append((start, prev))
    return intervals


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--yaml", nargs="+", required=True,
                         help="Translation YAML file(s) to scan (glob-expanded by the shell is fine).")
    parser.add_argument("--only-keys", action="append",
                         help="Restrict scanning to these top-level YAML keys (e.g. _language_name). Repeatable. "
                              "Default: scan every string in the file.")
    parser.add_argument("--exclude-cmap", required=True, help="TTF/OTF whose cmap is already covered; its codepoints are skipped.")
    parser.add_argument("--min-codepoint", type=lambda s: int(s, 0), default=0x80, help="Ignore codepoints below this (default: skip ASCII).")
    parser.add_argument("--codepoint-range", action="append", metavar="MIN,MAX",
                         help="Only keep codepoints within this inclusive hex range (e.g. 0x4E00,0x9FFF). "
                              "Repeatable; a codepoint matching any range is kept. Default: no range filter.")
    args = parser.parse_args()

    chars = collect_chars(args.yaml, args.only_keys)
    codepoints = {ord(c) for c in chars if ord(c) >= args.min_codepoint}

    if args.codepoint_range:
        ranges = []
        for spec in args.codepoint_range:
            lo_s, hi_s = spec.split(",")
            ranges.append((int(lo_s, 0), int(hi_s, 0)))
        codepoints = {cp for cp in codepoints if any(lo <= cp <= hi for lo, hi in ranges)}

    base = TTFont(args.exclude_cmap)
    base_cmap = set(base.getBestCmap().keys())

    missing = codepoints - base_cmap
    intervals = coalesce(missing)

    for lo, hi in intervals:
        print(f"0x{lo:04X},0x{hi:04X}")

    print(f"# {len(missing)} codepoints in {len(intervals)} intervals", file=sys.stderr)


if __name__ == "__main__":
    main()
