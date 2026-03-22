#!/usr/bin/env python3
"""
dict_coverage_check.py — Offline dictionary coverage audit.

Reads all headwords from a StarDict .idx file (and optionally alt-forms from
.syn) and compares them against all words extracted from an epub. Reports which
epub words are absent from the dictionary entirely.

Usage:
    python3 scripts/dict_coverage_check.py <dict-folder> <epub-file> [--syn]

Example:
    python3 scripts/dict_coverage_check.py example.dicts/dict-en-en-2025 \\
        test/epubs/test_dictionary.epub --syn

Output:
    Headword count, epub word count, found/not-found breakdown, sorted
    not-found list.

Notes:
    - Comparison is case-insensitive (matches firmware cleanWord() behaviour).
    - Stemming is NOT replicated — a word reachable only via stem variant
      (e.g. "harvested" → "harvest") will appear as not-found here.
    - .dict / .dict.dz is never read — only .idx and .syn are needed.
"""

import argparse
import re
import sys
import zipfile
from pathlib import Path
from xml.etree.ElementTree import fromstring


# ---------------------------------------------------------------------------
# StarDict binary readers
# ---------------------------------------------------------------------------

def read_idx_words(idx_path: Path) -> set:
    """Return lowercased set of all headwords from a StarDict .idx file.

    Binary format per entry: word\\0 + uint32_BE(offset) + uint32_BE(size)
    """
    words = set()
    data = idx_path.read_bytes()
    i = 0
    while i < len(data):
        null = data.index(b'\x00', i)
        word = data[i:null].decode('utf-8', errors='replace').lower()
        words.add(word)
        i = null + 1 + 8  # null byte + 4-byte offset + 4-byte size
    return words


def read_syn_words(syn_path: Path) -> set:
    """Return lowercased set of all synonym words from a StarDict .syn file.

    Binary format per entry: word\\0 + uint32_BE(idx_ordinal)
    """
    words = set()
    data = syn_path.read_bytes()
    i = 0
    while i < len(data):
        null = data.index(b'\x00', i)
        word = data[i:null].decode('utf-8', errors='replace').lower()
        words.add(word)
        i = null + 1 + 4  # null byte + 4-byte ordinal
    return words


# ---------------------------------------------------------------------------
# Epub word extraction
# ---------------------------------------------------------------------------

_TOKEN_RE = re.compile(r"[a-zA-Z'\u2019\-]+")
_NS_RE = re.compile(r'\s+xmlns(?::\w+)?="[^"]*"')


def epub_words(epub_path: Path) -> set:
    """Return lowercased set of all word tokens extracted from epub XHTML chapters."""
    words = set()
    with zipfile.ZipFile(epub_path) as zf:
        for name in zf.namelist():
            if not (name.endswith('.xhtml') or name.endswith('.html')):
                continue
            raw = zf.read(name).decode('utf-8', errors='replace')
            # Strip XML namespace declarations so ElementTree can parse without
            # namespace-qualified tags
            raw = _NS_RE.sub('', raw)
            try:
                root = fromstring(raw)
            except Exception:
                continue
            text = ' '.join(root.itertext())
            for tok in _TOKEN_RE.findall(text):
                word = tok.lower().strip("'\u2019-")
                if word:
                    words.add(word)
    return words


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def find_base_name(dict_folder: Path) -> str:
    """Return the base stem name from the single .ifo file in dict_folder."""
    ifo_files = list(dict_folder.glob('*.ifo'))
    if not ifo_files:
        print(f"error: no .ifo file found in {dict_folder}", file=sys.stderr)
        sys.exit(1)
    if len(ifo_files) > 1:
        # Pick the one whose stem matches the folder name if possible, else first
        folder_name = dict_folder.name
        match = [f for f in ifo_files if f.stem == folder_name]
        return (match[0] if match else ifo_files[0]).stem
    return ifo_files[0].stem


def main():
    parser = argparse.ArgumentParser(
        description='Check which epub words are missing from a StarDict dictionary.'
    )
    parser.add_argument('dict_folder', help='Path to StarDict dictionary folder')
    parser.add_argument('epub_file', help='Path to epub file')
    parser.add_argument(
        '--syn', action='store_true',
        help='Also include .syn alt-form words in the reachability check'
    )
    args = parser.parse_args()

    dict_folder = Path(args.dict_folder)
    epub_path = Path(args.epub_file)

    if not dict_folder.is_dir():
        print(f"error: dict folder not found: {dict_folder}", file=sys.stderr)
        sys.exit(1)
    if not epub_path.is_file():
        print(f"error: epub file not found: {epub_path}", file=sys.stderr)
        sys.exit(1)

    base = find_base_name(dict_folder)
    idx_path = dict_folder / f'{base}.idx'
    syn_path = dict_folder / f'{base}.syn'

    if not idx_path.exists():
        print(f"error: .idx not found: {idx_path}", file=sys.stderr)
        sys.exit(1)

    # --- Load dictionary words ---
    print(f"Reading headwords from {idx_path} ...", end=' ', flush=True)
    headwords = read_idx_words(idx_path)
    print(f"{len(headwords):,}")

    syn_words: set = set()
    if args.syn:
        if syn_path.exists():
            print(f"Reading syn words from {syn_path} ...", end=' ', flush=True)
            syn_words = read_syn_words(syn_path)
            print(f"{len(syn_words):,}")
        else:
            print(f"warning: --syn requested but no .syn file found: {syn_path}", file=sys.stderr)

    all_dict_words = headwords | syn_words

    # --- Extract epub words ---
    print(f"Extracting words from {epub_path} ...", end=' ', flush=True)
    words = epub_words(epub_path)
    print(f"{len(words):,}")

    # --- Compare ---
    found_headword = {w for w in words if w in headwords}
    found_syn_only = {w for w in words if w not in headwords and w in syn_words}
    not_found = words - all_dict_words

    total = len(words)

    print()
    print(f"{'Dictionary folder:':<28} {dict_folder.name}")
    print(f"{'Epub file:':<28} {epub_path.name}")
    print()
    print(f"{'Headwords in dict:':<28} {len(headwords):>8,}")
    if args.syn:
        print(f"{'Syn words in dict:':<28} {len(syn_words):>8,}")
    print(f"{'Unique words in epub:':<28} {total:>8,}")
    print()
    print(f"{'Found as headword:':<28} {len(found_headword):>8,}  ({100*len(found_headword)/total:.1f}%)")
    if args.syn:
        print(f"{'Found via .syn only:':<28} {len(found_syn_only):>8,}  ({100*len(found_syn_only)/total:.1f}%)")
    print(f"{'NOT FOUND:':<28} {len(not_found):>8,}  ({100*len(not_found)/total:.1f}%)")

    if not_found:
        print()
        print(f"--- Not found ({len(not_found)} words) ---")
        for word in sorted(not_found):
            print(f"  {word}")


if __name__ == '__main__':
    main()
