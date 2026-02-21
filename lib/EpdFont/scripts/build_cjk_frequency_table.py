#!/usr/bin/env python3
"""
Build a merged CJK frequency table from Chinese character frequency data
and Japanese Jōyō kanji list.

Output: TSV file with codepoint<TAB>frequency_rank where rank 1 = most frequent.

Sources:
- hanzi_frequency.csv: Jun Da's Modern Chinese Character Frequency List (via hanziDB)
- joyo_kanji.csv: Japanese Jōyō kanji (2,136 government-standard characters)
- Hiragana (U+3040-U+309F) and Katakana (U+30A0-U+30FF): always high priority
- CJK Punctuation (U+3000-U+303F): high priority
- Hangul Syllables: Unicode ordering for the KS X 1001 subset
"""

import csv
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(SCRIPT_DIR, "data")

HANZI_FILE = os.path.join(DATA_DIR, "hanzi_frequency.csv")
JOYO_FILE = os.path.join(DATA_DIR, "joyo_kanji.csv")
TRAD_SIMP_FILE = os.path.join(DATA_DIR, "trad_to_simp.tsv")
OUTPUT_FILE = os.path.join(DATA_DIR, "cjk_frequency.tsv")


def load_hanzi_frequency():
    """Load Chinese character frequency rankings. Returns dict: codepoint -> rank."""
    freq = {}
    with open(HANZI_FILE, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                rank = int(row["frequency_rank"])
                char = row["character"]
                if len(char) == 1:
                    cp = ord(char)
                    freq[cp] = rank
            except (ValueError, KeyError):
                continue
    return freq


def load_joyo_kanji():
    """Load Jōyō kanji list. Returns set of codepoints."""
    kanji = set()
    with open(JOYO_FILE, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            # Format: "kanji reading reading_type" (space-separated)
            char = line[0]
            if len(char) == 1:
                kanji.add(ord(char))
    return kanji


def load_trad_to_simp():
    """Load Traditional→Simplified mapping. Returns dict: trad_cp -> simp_cp."""
    mapping = {}
    if not os.path.exists(TRAD_SIMP_FILE):
        return mapping
    with open(TRAD_SIMP_FILE, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) >= 2:
                trad_cp = int(parts[0], 0)
                simp_cp = int(parts[1], 0)
                mapping[trad_cp] = simp_cp
    return mapping


def main():
    print("Loading Chinese character frequency data...")
    hanzi_freq = load_hanzi_frequency()
    print(f"  Loaded {len(hanzi_freq)} characters")

    print("Loading Jōyō kanji list...")
    joyo_kanji = load_joyo_kanji()
    print(f"  Loaded {len(joyo_kanji)} kanji")

    print("Loading Traditional→Simplified mapping...")
    trad_to_simp = load_trad_to_simp()
    print(f"  Loaded {len(trad_to_simp)} mappings")

    # Build reverse mapping: simplified -> list of traditional codepoints
    simp_to_trad = {}
    for trad_cp, simp_cp in trad_to_simp.items():
        simp_to_trad.setdefault(simp_cp, []).append(trad_cp)

    # Start building the ranked list
    ranked = []  # list of (codepoint, rank)

    # 1. CJK Punctuation (U+3000-U+303F) — highest priority
    rank = 1
    for cp in range(0x3000, 0x303F + 1):
        ranked.append((cp, rank))
        rank += 1

    # 2. Hiragana (U+3040-U+309F) — always high priority
    for cp in range(0x3040, 0x309F + 1):
        ranked.append((cp, rank))
        rank += 1

    # 3. Katakana (U+30A0-U+30FF) — always high priority
    for cp in range(0x30A0, 0x30FF + 1):
        ranked.append((cp, rank))
        rank += 1

    # 4. Chinese characters by frequency, with Traditional variants inserted
    # after their Simplified equivalents
    sorted_hanzi = sorted(hanzi_freq.items(), key=lambda x: x[1])
    existing_cps = {cp for cp, _ in ranked}
    trad_inserted = 0
    for cp, _original_rank in sorted_hanzi:
        if cp in existing_cps:
            continue  # Already inserted as a Traditional variant of an earlier character
        ranked.append((cp, rank))
        existing_cps.add(cp)
        rank += 1
        # Insert Traditional variants right after their Simplified equivalent
        for trad_cp in sorted(simp_to_trad.get(cp, [])):
            if trad_cp not in existing_cps:
                ranked.append((trad_cp, rank))
                existing_cps.add(trad_cp)
                rank += 1
                trad_inserted += 1

    print(f"  Inserted {trad_inserted} Traditional variants after Simplified equivalents")

    # 5. Jōyō kanji not already in the list
    extra_joyo = sorted(joyo_kanji - existing_cps)
    for cp in extra_joyo:
        ranked.append((cp, rank))
        existing_cps.add(cp)
        rank += 1

    # 6. Hangul Syllables (U+AC00-U+D7AF) — Unicode ordering
    # The KS X 1001 subset covers ~2,350 syllables but identifying them requires
    # a mapping table. For now, include the full block in Unicode order which
    # groups by initial consonant (providing natural locality).
    for cp in range(0xAC00, 0xD7AF + 1):
        ranked.append((cp, rank))
        rank += 1

    # 7. Halfwidth/Fullwidth Forms (U+FF00-U+FFEF)
    for cp in range(0xFF00, 0xFFEF + 1):
        ranked.append((cp, rank))
        rank += 1

    # Write output
    with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
        f.write("# CJK frequency table: codepoint<TAB>rank (1 = most frequent)\n")
        f.write("# Generated by build_cjk_frequency_table.py\n")
        for cp, r in ranked:
            f.write(f"0x{cp:04X}\t{r}\n")

    print(f"\nOutput: {OUTPUT_FILE}")
    print(f"Total entries: {len(ranked)}")
    print(f"  CJK Punctuation: {0x303F - 0x3000 + 1}")
    print(f"  Hiragana: {0x309F - 0x3040 + 1}")
    print(f"  Katakana: {0x30FF - 0x30A0 + 1}")
    print(f"  Chinese chars (by frequency): {len(sorted_hanzi)}")
    print(f"  Traditional variants inserted: {trad_inserted}")
    print(f"  Extra Jōyō kanji: {len(extra_joyo)}")
    print(f"  Hangul syllables: {0xD7AF - 0xAC00 + 1}")
    print(f"  Halfwidth/Fullwidth forms: {0xFFEF - 0xFF00 + 1}")


if __name__ == "__main__":
    main()
