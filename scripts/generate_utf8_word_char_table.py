#!/usr/bin/env python3
"""Generate lib/Utf8/Utf8WordCharTable.h.

Walks every Unicode codepoint and keeps the ones whose general category is
Letter, Number, or Mark (L*, N*, M*), then coalesces them into contiguous
[start, end] ranges. This is the whitelist used by word-edge trimming: a
codepoint is "part of a word" iff it falls in one of these ranges.

Requires Python 3.14.7 (Unicode 16.0.0) to match the committed table exactly;
a different unicodedata version will produce a different set of ranges.

Usage:
    python3 scripts/generate_utf8_word_char_table.py > lib/Utf8/Utf8WordCharTable.h
    ./bin/clang-format-fix -g
"""

import sys
import unicodedata

MAX_CODEPOINT = 0x110000  # exclusive
WORD_CATEGORY_PREFIXES = ("L", "N", "M")

HEADER = """// Auto-generated table of Unicode codepoint ranges classified as Letter,
// Number, or Mark (general categories L*, N*, M*), derived from Python
// 3.12.3's unicodedata (Unicode 15.0.0).
//
// This is the whitelist side of word-edge trimming: a codepoint is "part of a
// word" iff it falls in one of these ranges. Everything else (punctuation,
// symbols, separators, format/control characters, in any script) is
// considered a boundary and stripped from word edges — no per-script or
// per-punctuation-mark list to maintain, and no gaps like the guillemet/CJK
// punctuation blocks a hand-picked blacklist previously missed. Marks (M*)
// are included so combining diacritics stay attached to their base letter
// instead of being treated as trailing punctuation.
#pragma once
#include <cstdint>

struct Utf8WordCharRange {
  uint32_t start;
  uint32_t end;
};

// Sorted by start for binary search.
static constexpr Utf8WordCharRange kUtf8WordCharTable[] = {
"""

FOOTER = """};

static constexpr int kUtf8WordCharTableSize = sizeof(kUtf8WordCharTable) / sizeof(kUtf8WordCharTable[0]);
"""


def is_word_codepoint(codepoint):
    return unicodedata.category(chr(codepoint))[0] in WORD_CATEGORY_PREFIXES


def compute_ranges():
    ranges = []
    start = None
    for codepoint in range(MAX_CODEPOINT):
        if is_word_codepoint(codepoint):
            if start is None:
                start = codepoint
        elif start is not None:
            ranges.append((start, codepoint - 1))
            start = None
    if start is not None:
        ranges.append((start, MAX_CODEPOINT - 1))
    return ranges


def format_entries(ranges):
    entries = [f"{{0x{start:06X}, 0x{end:06X}}}" for start, end in ranges]
    lines = []
    for i in range(0, len(entries), 5):
        row = ", ".join(entries[i : i + 5])
        lines.append(f"    {row},")
    return "\n".join(lines)


def main():
    if sys.version_info[:3] != (3, 14, 7) or unicodedata.unidata_version != "16.0.0":
        print(
            f"warning: running Python {sys.version.split()[0]} / Unicode "
            f"{unicodedata.unidata_version}, expected 3.12.3 / 15.0.0 — output "
            "may not match the committed table",
            file=sys.stderr,
        )

    ranges = compute_ranges()
    sys.stdout.write(HEADER)
    sys.stdout.write(format_entries(ranges))
    sys.stdout.write("\n")
    sys.stdout.write(FOOTER)


if __name__ == "__main__":
    main()
