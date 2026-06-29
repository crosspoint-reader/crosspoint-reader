#!/usr/bin/env python3
"""Audit StarDict dictionary Unicode codepoints and compare them dynamically against
the compiled built-in fonts (.h files under lib/EpdFont/builtinFonts) to report missing symbols.

Usage:
    python3 scripts/dict_audit_symbols.py --dict-path fs_/.dictionaries/gcide/stardict
"""

import argparse
import collections
from pathlib import Path
import re
import sys
import unicodedata

def get_unicode_block(cp: int) -> str:
    blocks = [
        (0x0000, 0x007F, "Basic Latin"),
        (0x0080, 0x00FF, "Latin-1 Supplement"),
        (0x0100, 0x017F, "Latin Extended-A"),
        (0x0180, 0x024F, "Latin Extended-B"),
        (0x0250, 0x02AF, "IPA Extensions"),
        (0x02B0, 0x02FF, "Spacing Modifier Letters"),
        (0x0300, 0x036F, "Combining Diacritical Marks"),
        (0x0370, 0x03FF, "Greek and Coptic"),
        (0x0400, 0x04FF, "Cyrillic"),
        (0x0500, 0x052F, "Cyrillic Supplement"),
        (0x0530, 0x058F, "Armenian"),
        (0x0590, 0x05FF, "Hebrew"),
        (0x1D00, 0x1D7F, "Phonetic Extensions"),
        (0x1D80, 0x1DBF, "Phonetic Extensions Supplement"),
        (0x1DC0, 0x1DFF, "Combining Diacritical Marks Supplement"),
        (0x1E00, 0x1EFF, "Latin Extended Additional"),
        (0x1F00, 0x1FFF, "Greek Extended"),
        (0x2000, 0x206F, "General Punctuation"),
        (0x2070, 0x209F, "Superscripts and Subscripts"),
        (0x20A0, 0x20CF, "Currency Symbols"),
        (0x20D0, 0x20FF, "Combining Diacritical Marks for Symbols"),
        (0x2100, 0x214F, "Letterlike Symbols"),
        (0x2150, 0x218F, "Number Forms"),
        (0x2190, 0x21FF, "Arrows"),
        (0x2200, 0x22FF, "Mathematical Operators"),
        (0x2300, 0x23FF, "Miscellaneous Technical"),
        (0x2400, 0x243F, "Control Pictures"),
        (0x2440, 0x245F, "Optical Character Recognition"),
        (0x2460, 0x24FF, "Enclosed Alphanumerics"),
        (0x2500, 0x257F, "Box Drawing"),
        (0x2580, 0x259F, "Block Elements"),
        (0x25A0, 0x25FF, "Geometric Shapes"),
        (0x2600, 0x26FF, "Miscellaneous Symbols"),
        (0x2700, 0x27BF, "Dingbats"),
        (0x27C0, 0x27EF, "Miscellaneous Mathematical Symbols-A"),
        (0x27F0, 0x27FF, "Supplemental Arrows-A"),
        (0x2800, 0x28FF, "Braille Patterns"),
        (0x2900, 0x29FF, "Supplemental Arrows-B"),
        (0x2A00, 0x2AFF, "Supplemental Mathematical Operators"),
        (0x2B00, 0x2BFF, "Miscellaneous Symbols and Arrows"),
        (0x2E00, 0x2E7F, "Supplemental Punctuation"),
        (0x3000, 0x303F, "CJK Symbols and Punctuation"),
        (0xFB00, 0xFB4F, "Alphabetic Presentation Forms"),
        (0xFF00, 0xFFEF, "Halfwidth and Fullwidth Forms"),
    ]
    for start, end, name in blocks:
        if start <= cp <= end:
            return name
    return "Other / Uncategorized"

def build_ranges(codepoints: list[int]) -> list[tuple[int, int]]:
    """Group contiguous codepoints into ranges."""
    if not codepoints:
        return []
    
    sorted_cps = sorted(codepoints)
    ranges = []
    start = sorted_cps[0]
    prev = sorted_cps[0]
    
    for cp in sorted_cps[1:]:
        if cp == prev + 1:
            prev = cp
        else:
            ranges.append((start, prev))
            start = cp
            prev = cp
    ranges.append((start, prev))
    return ranges

def load_supported_font_codepoints(builtin_fonts_dir: Path) -> dict[str, set[int]]:
    """Scan all .h files in the directory and parse their Interval structures."""
    font_support = {}
    
    if not builtin_fonts_dir.is_dir():
        print(f"Warning: Built-in fonts directory not found at {builtin_fonts_dir}", file=sys.stderr)
        return {}

    for h_file in sorted(builtin_fonts_dir.glob("*.h")):
        if h_file.name == "all.h":
            continue
        try:
            content = h_file.read_text(encoding="utf-8")
            # Extract Interval array declaration block
            match = re.search(r"static const EpdUnicodeInterval \w+Intervals\[\] = \{(.*?)\};", content, re.DOTALL)
            if match:
                interval_text = match.group(1)
                codepoints = set()
                # Parse each interval: { start, end, offset }
                for line in interval_text.splitlines():
                    m = re.search(r"\{\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*\}", line)
                    if m:
                        start = int(m.group(1), 16)
                        end = int(m.group(2), 16)
                        for cp in range(start, end + 1):
                            codepoints.add(cp)
                font_support[h_file.stem] = codepoints
        except Exception as e:
            print(f"Error parsing font header {h_file.name}: {e}", file=sys.stderr)
            sys.exit(1)

    return font_support

def main():
    parser = argparse.ArgumentParser(description="Audit StarDict dictionary Unicode symbols against built-in fonts")
    parser.add_argument(
        "--dict-path",
        required=True,
        help="Base path or directory to StarDict dictionary files (e.g. fs_/.dictionaries/gcide/stardict)",
    )
    parser.add_argument(
        "--min-freq",
        type=int,
        default=2,
        help="Minimum frequency to suggest support for a symbol (default: 2)",
    )
    args = parser.parse_args()

    workspace_root = Path(__file__).resolve().parent.parent
    dict_path = Path(args.dict_path)
    
    # Resolve files based on dict_path
    if dict_path.is_dir():
        files_to_scan = []
        for ext in ("*.dict", "*.idx", "*.syn"):
            files_to_scan.extend(dict_path.glob(ext))
    else:
        files_to_scan = []
        parent = dict_path.parent
        stem = dict_path.name
        for ext in (".dict", ".idx", ".syn"):
            f = parent / f"{stem}{ext}"
            if f.exists():
                files_to_scan.append(f)

    if not files_to_scan:
        print(f"Error: No .dict, .idx, or .syn files found for path {dict_path}", file=sys.stderr)
        sys.exit(1)

    # 1. Parse dictionary content and count frequencies
    print("Scanning files:")
    for f in files_to_scan:
        print(f"  - {f}")

    counter = collections.Counter()
    for fpath in files_to_scan:
        print(f"Parsing {fpath.name} ({fpath.stat().st_size / 1024 / 1024:.2f} MB)...")
        try:
            data = fpath.read_bytes()
            if fpath.suffix == ".dict":
                text = data.decode("utf-8", errors="replace")
                for char in text:
                    cp = ord(char)
                    if cp >= 0x80:
                        counter[cp] += 1
            elif fpath.suffix == ".idx":
                pos = 0
                while pos < len(data):
                    null = data.index(b"\x00", pos)
                    word = data[pos:null].decode("utf-8", errors="replace")
                    for char in word:
                        cp = ord(char)
                        if cp >= 0x80:
                            counter[cp] += 1
                    pos = null + 1 + 8
            elif fpath.suffix == ".syn":
                pos = 0
                while pos < len(data):
                    null = data.index(b"\x00", pos)
                    synonym = data[pos:null].decode("utf-8", errors="replace")
                    for char in synonym:
                        cp = ord(char)
                        if cp >= 0x80:
                            counter[cp] += 1
                    pos = null + 1 + 4
        except Exception as e:
            print(f"Error parsing {fpath.name}: {e}", file=sys.stderr)
            sys.exit(1)

    total_non_ascii_chars = sum(counter.values())
    unique_non_ascii_chars = len(counter)
    print(f"\nScan complete. Found {total_non_ascii_chars} non-ASCII characters, {unique_non_ascii_chars} unique.")

    # 2. Load compiled built-in fonts support ranges dynamically
    builtin_fonts_dir = workspace_root / "lib" / "EpdFont" / "builtinFonts"
    print(f"Parsing built-in font header files in {builtin_fonts_dir}...")
    font_support = load_supported_font_codepoints(builtin_fonts_dir)
    print(f"Loaded {len(font_support)} built-in font headers.")

    # Union of all supported codepoints from ALL built-in fonts
    all_supported_cps = set()
    for font_cps in font_support.values():
        all_supported_cps.update(font_cps)

    # 3. Perform audit
    unsupported_chars = []
    supported_chars = []

    for cp, count in sorted(counter.items()):
        supported = cp in all_supported_cps
        char_name = unicodedata.name(chr(cp), "UNKNOWN NAME")
        char_repr = chr(cp) if not unicodedata.combining(chr(cp)) else f" {chr(cp)}"
        
        info = {
            "codepoint": cp,
            "char": char_repr,
            "name": char_name,
            "count": count,
            "block": get_unicode_block(cp)
        }
        
        if supported:
            supported_chars.append(info)
        else:
            unsupported_chars.append(info)

    # Output report
    print("\n" + "="*80)
    print("AUDIT REPORT: MISSING CHARACTERS IN COMPILED BUILT-IN FONTS")
    print("="*80)
    
    print(f"\nTotal unique non-ASCII characters found in dictionary: {unique_non_ascii_chars}")
    print(f"Supported in at least one built-in font: {len(supported_chars)}")
    print(f"Missing (unsupported in ALL built-in fonts): {len(unsupported_chars)}")

    if unsupported_chars:
        print("\n--- Missing Characters ---")
        print(f"{'Char':<6} {'Codepoint':<10} {'Freq':<8} {'Block':<30} {'Name':<30}")
        print("-" * 90)
        for info in sorted(unsupported_chars, key=lambda x: x["count"], reverse=True):
            print(f"{info['char']:<6} U+{info['codepoint']:04X}     {info['count']:<8} {info['block']:<30} {info['name']:<30}")
    else:
        print("\nAll dictionary characters are 100% supported by the compiled built-in fonts!")

    # 4. Generate suggested ranges for missing characters
    to_suggest = [info["codepoint"] for info in unsupported_chars if info["count"] >= args.min_freq]
    suggested_ranges = build_ranges(to_suggest)

    print("\n" + "="*80)
    print(f"SUGGESTED RANGES FOR MISSING SYMBOLS (Minimum Frequency >= {args.min_freq})")
    print("="*80)
    
    if not suggested_ranges:
        print("No new ranges to suggest!")
        return

    print(f"Number of suggested ranges/groups: {len(suggested_ranges)}")
    for start, end in suggested_ranges:
        if start == end:
            print(f"  - U+{start:04X} ({chr(start)}) [Freq: {counter[start]}]")
        else:
            total_freq = sum(counter[cp] for cp in range(start, end + 1))
            print(f"  - U+{start:04X}-U+{end:04X} ({chr(start)}-{chr(end)}) [Total Freq: {total_freq}]")

    # Format for convert-builtin-fonts.sh
    print("\n--- Copy-paste for convert-builtin-fonts.sh ---")
    
    unicode_strs = []
    for start, end in suggested_ranges:
        if start == end:
            unicode_strs.append(f"U+{start:04X}")
        else:
            unicode_strs.append(f"U+{start:04X}-U+{end:04X}")
    print("Append to DICT_FONT UNICODES (ensure_stripped_font call in convert-builtin-fonts.sh):")
    print(f'  {",".join(unicode_strs)}')
    
    print("\nAppend to fontconvert.py arguments:")
    for start, end in suggested_ranges:
        print(f"  --additional-intervals 0x{start:04X},0x{end:04X} \\")

    # Format for IpaUtils.h
    print("\n--- Copy-paste for isIpaCodepoint in IpaUtils.h ---")
    cpp_conditions = []
    for start, end in suggested_ranges:
        if start == end:
            cpp_conditions.append(f"cp == 0x{start:04X}")
        else:
            cpp_conditions.append(f"(cp >= 0x{start:04X} && cp <= 0x{end:04X})")
    
    print("Append to return expression:")
    print(f"  {' || '.join(cpp_conditions)}")

if __name__ == "__main__":
    main()
