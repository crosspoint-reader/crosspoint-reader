#!/usr/bin/env python3
"""Generate lib/Utf8/Utf8CaseFoldTable.h.

Walks every Unicode codepoint whose str.casefold() yields exactly one different
codepoint and coalesces them into [start, end] ranges that step by 1 or 2 with
a constant (folded - codepoint) delta. Codepoints whose fold expands to several
codepoints (e.g. U+1E9E -> "ss") are skipped: a codepoint->codepoint fold
can't represent them.

Requires a Python whose unicodedata is Unicode 16.0.0 (e.g. Python 3.14.x) to
match the committed table exactly (and the word char table); a different
Unicode version will produce a different set of ranges.

Usage:
    python3 scripts/generate_utf8_case_fold_table.py > lib/Utf8/Utf8CaseFoldTable.h
    ./bin/clang-format-fix -g
"""

import sys
import unicodedata

MAX_CODEPOINT = 0x110000  # exclusive
SINGLETON_STEP = 2  # step is irrelevant for one-entry ranges; keep the historic value
ENTRIES_PER_ROW = 3

HEADER = """// Auto-generated simple (single-codepoint) Unicode case-folding table,
// derived from Python 3.14's unicodedata (Unicode 16.0.0): for every
// codepoint whose str.casefold() yields exactly one codepoint, record
// [start, end] stepping by 1 or 2 with a constant (folded - cp) delta. This is
// script-agnostic by construction — it covers every alphabet with a simple
// case pairing (Latin, Greek, Cyrillic, Armenian, Cherokee, Deseret, ...)
// without a single hardcoded script range in the caller.
//
// Deliberately CASE FOLDING (str.casefold()), not lowercasing (str.lower()):
// this table backs a comparison/search key only, never a displayed string, so
// a fold that isn't a valid *display* lowercase is fine — and is in fact
// required: Greek final sigma U+03C2 (ς) case-folds to U+03C3 (σ), so a
// capitalized word ending in Σ matches a headword spelled with the
// context-sensitive final-sigma form. Plain lower() can't do this — it leaves
// ς unchanged, since it's already "lowercase".
//
// Special casings that expand to multiple codepoints (e.g. German capital
// ẞ -> "ss") are skipped: those need multi-codepoint output, which a
// codepoint->codepoint fold can't represent, and this table only needs a
// stable single-codepoint key.
#pragma once
#include <cstdint>

struct Utf8CaseFoldRange {
  uint32_t start;
  uint32_t end;
  uint8_t step;
  int32_t delta;
};

// Sorted by start for binary search.
static constexpr Utf8CaseFoldRange kUtf8CaseFoldTable[] = {
"""

FOOTER = """};

static constexpr int kUtf8CaseFoldTableSize = sizeof(kUtf8CaseFoldTable) / sizeof(kUtf8CaseFoldTable[0]);
"""


def compute_deltas():
    """Map codepoint -> (folded - codepoint) for every simple, changing fold."""
    deltas = {}
    for codepoint in range(MAX_CODEPOINT):
        if 0xD800 <= codepoint <= 0xDFFF:  # surrogates are not scalar values
            continue
        folded = chr(codepoint).casefold()
        if len(folded) == 1 and folded != chr(codepoint):
            deltas[codepoint] = ord(folded) - codepoint
    return deltas


def compute_ranges(deltas):
    ranges = []
    codepoints = sorted(deltas)
    i = 0
    while i < len(codepoints):
        start = codepoints[i]
        delta = deltas[start]
        nxt = codepoints[i + 1] if i + 1 < len(codepoints) else None

        # Pick the step from the next mapped codepoint. A step-2 range is only
        # valid if the codepoint in between has no fold of its own, since the
        # runtime treats off-step codepoints as unmapped.
        if nxt == start + 1 and deltas[nxt] == delta:
            step = 1
        elif nxt == start + 2 and deltas[nxt] == delta:
            step = 2
        else:
            step = SINGLETON_STEP

        end = start
        j = i + 1
        while (
            j < len(codepoints)
            and codepoints[j] == end + step
            and deltas[codepoints[j]] == delta
            and (step == 1 or (end + 1) not in deltas)
        ):
            end = codepoints[j]
            j += 1
        ranges.append((start, end, step, delta))
        i = j
    return ranges


def format_entries(ranges):
    entries = [f"{{0x{start:06X}, 0x{end:06X}, {step}, {delta}}}" for start, end, step, delta in ranges]
    lines = []
    for i in range(0, len(entries), ENTRIES_PER_ROW):
        row = ", ".join(entries[i : i + ENTRIES_PER_ROW])
        lines.append(f"    {row},")
    return "\n".join(lines)


def verify(ranges, deltas):
    """Re-implement the runtime lookup and check it reproduces every fold."""
    mapped = {}
    for start, end, step, delta in ranges:
        for codepoint in range(start, end + 1, step):
            assert codepoint not in mapped, f"overlap at U+{codepoint:04X}"
            mapped[codepoint] = delta
    assert mapped == deltas, "ranges do not reproduce the fold mapping"


def main():
    if unicodedata.unidata_version != "16.0.0":
        print(
            f"warning: running Python {sys.version.split()[0]} / Unicode "
            f"{unicodedata.unidata_version}, expected Unicode 16.0.0 — output "
            "may not match the committed table",
            file=sys.stderr,
        )

    deltas = compute_deltas()
    ranges = compute_ranges(deltas)
    verify(ranges, deltas)
    sys.stdout.write(HEADER)
    sys.stdout.write(format_entries(ranges))
    sys.stdout.write("\n")
    sys.stdout.write(FOOTER)


if __name__ == "__main__":
    main()
