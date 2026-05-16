#!/usr/bin/env python3
"""
Build dictionary files for shortbread from WordNet 3.1.

Downloads WordNet, parses glosses, emits three files for SD card:
  /shortbread/dict/pages.idx  — RAM-loaded page table (sorted first words)
  /shortbread/dict/words.idx  — sorted variable-length records, 4KB pages
  /shortbread/dict/defs.bin   — raw DEFLATE-compressed definitions

Format details — keep in sync with src/util/DictLookup.cpp:

pages.idx:
  magic:        4 bytes  "DICT"
  version:      1 byte   = 1
  page_size:    2 bytes  little-endian, = 4096
  page_count:   4 bytes  little-endian
  entry_count:  4 bytes  little-endian (total words)
  then page_count entries, each:
    word_len:   1 byte
    word_bytes: word_len bytes (UTF-8, lowercase)

words.idx:
  page_count * 4096 bytes total. Each 4KB page holds:
    sequence of records, packed:
      word_len:   1 byte  (0 = page terminator / padding follows)
      word_bytes: word_len bytes
      def_off:    4 bytes little-endian   (offset into defs.bin)
      def_len:    4 bytes little-endian   (compressed length)
    after last record, remaining bytes in page are zero-padded.

defs.bin:
  concatenation of raw-DEFLATE-compressed UTF-8 gloss strings.
  Each entry decompresses to one human-readable definition block,
  e.g. "n. a small carnivorous mammal...\nv. to move stealthily...".
"""

from __future__ import annotations

import argparse
import io
import os
import struct
import sys
import tarfile
import urllib.request
import zlib
from collections import defaultdict
from pathlib import Path

WORDNET_URL = "https://wordnetcode.princeton.edu/wn3.1.dict.tar.gz"
PAGE_SIZE = 4096
POS_LABEL = {"n": "n.", "v": "v.", "a": "adj.", "s": "adj.", "r": "adv."}
DATA_FILES = ["data.noun", "data.verb", "data.adj", "data.adv"]


def fetch_wordnet(cache_dir: Path) -> Path:
    cache_dir.mkdir(parents=True, exist_ok=True)
    archive = cache_dir / "wn3.1.dict.tar.gz"
    if not archive.exists():
        print(f"Downloading WordNet 3.1 from {WORDNET_URL}", file=sys.stderr)
        urllib.request.urlretrieve(WORDNET_URL, archive)
    extracted = cache_dir / "dict"
    if not extracted.exists():
        print(f"Extracting {archive}", file=sys.stderr)
        with tarfile.open(archive) as tf:
            tf.extractall(cache_dir)
    return extracted


def format_gloss(raw: str) -> str:
    """Clean up a WordNet gloss line. Keep definition + examples, normalize
    quote chars and whitespace."""
    parts = [p.strip() for p in raw.split(";") if p.strip()]
    if not parts:
        return ""
    out = [parts[0]]
    for ex in parts[1:]:
        if ex.startswith('"') and ex.endswith('"'):
            ex = ex.strip('"').strip()
            if ex:
                out.append(f'"{ex}"')
        else:
            out.append(ex)
    return " — ".join(out)


def parse_data_file(path: Path) -> list[tuple[str, str, str, list[str]]]:
    """Return list of (word_lowercase, pos_label, gloss, synonyms)."""
    out = []
    with path.open(encoding="utf-8") as f:
        for line in f:
            if line.startswith("  "):
                continue
            if "|" not in line:
                continue
            head, gloss = line.split("|", 1)
            tokens = head.split()
            if len(tokens) < 5:
                continue
            ss_type = tokens[2]
            try:
                w_cnt = int(tokens[3], 16)
            except ValueError:
                continue
            words = []
            for i in range(w_cnt):
                idx = 4 + i * 2
                if idx >= len(tokens):
                    break
                w = tokens[idx].replace("_", " ").lower()
                w = w.split("(")[0].strip()
                if w:
                    words.append(w)
            gloss_clean = format_gloss(gloss)
            label = POS_LABEL.get(ss_type, "")
            for w in words:
                synonyms = [s for s in words if s != w]
                out.append((w, label, gloss_clean, synonyms))
    return out


def build_entries(dict_dir: Path) -> dict[str, str]:
    """word -> combined gloss text."""
    senses: dict[str, list[tuple[str, str, list[str]]]] = defaultdict(list)
    for fn in DATA_FILES:
        path = dict_dir / fn
        if not path.exists():
            print(f"warning: {path} not found, skipping", file=sys.stderr)
            continue
        for word, label, gloss, synonyms in parse_data_file(path):
            senses[word].append((label, gloss, synonyms))
    result: dict[str, str] = {}
    for word, sense_list in senses.items():
        seen: set[tuple[str, str]] = set()
        lines: list[str] = []
        for label, gloss, synonyms in sense_list:
            key = (label, gloss)
            if key in seen:
                continue
            seen.add(key)
            prefix = f"{label} " if label else ""
            body = gloss
            if synonyms:
                # Keep at most 6 synonyms per sense to limit size.
                syn = ", ".join(synonyms[:6])
                body = f"{body}  [syn: {syn}]"
            lines.append(f"{prefix}{body}")
        result[word] = "\n".join(lines[:8])  # cap senses per word
    return result


def pack_words(entries: dict[str, str]) -> tuple[bytes, bytes, bytes, int]:
    """Return (pages_idx, words_idx, defs_blob, entry_count)."""
    sorted_words = sorted(entries.keys())

    defs_blob = bytearray()
    records: list[tuple[bytes, int, int]] = []
    for w in sorted_words:
        wb = w.encode("utf-8")
        if len(wb) > 255:
            continue
        gloss = entries[w].encode("utf-8")
        compressed = zlib.compress(gloss, level=9)
        compressed = compressed[2:-4]  # strip zlib header + adler32 -> raw DEFLATE
        if len(compressed) > 0xFFFFFFFF:
            continue
        def_off = len(defs_blob)
        defs_blob.extend(compressed)
        records.append((wb, def_off, len(compressed)))

    pages: list[bytearray] = [bytearray()]
    page_first_words: list[bytes] = []
    for wb, off, ln in records:
        rec = bytes([len(wb)]) + wb + struct.pack("<II", off, ln)
        if len(rec) >= PAGE_SIZE:
            raise RuntimeError(f"record too large for page: {wb!r}")
        if len(pages[-1]) + len(rec) > PAGE_SIZE:
            pages.append(bytearray())
        if not pages[-1]:
            page_first_words.append(wb)
        pages[-1].extend(rec)

    words_idx = bytearray()
    for page in pages:
        page = bytes(page)
        words_idx.extend(page)
        pad = PAGE_SIZE - len(page)
        if pad:
            words_idx.extend(b"\x00" * pad)

    page_count = len(pages)
    pages_idx = bytearray()
    pages_idx.extend(b"DICT")
    pages_idx.append(1)  # version
    pages_idx.extend(struct.pack("<H", PAGE_SIZE))
    pages_idx.extend(struct.pack("<I", page_count))
    pages_idx.extend(struct.pack("<I", len(records)))
    for wb in page_first_words:
        pages_idx.append(len(wb))
        pages_idx.extend(wb)

    return bytes(pages_idx), bytes(words_idx), bytes(defs_blob), len(records)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", default=".cache/wordnet",
                    help="dir to cache the WordNet download")
    ap.add_argument("--out", default="dict_out",
                    help="output dir (will contain pages.idx, words.idx, defs.bin)")
    ap.add_argument("--wordnet-dir",
                    help="path to extracted WordNet 'dict/' dir (skip download)")
    args = ap.parse_args()

    cache = Path(args.cache)
    if args.wordnet_dir:
        dict_dir = Path(args.wordnet_dir)
    else:
        dict_dir = fetch_wordnet(cache)
    print(f"Reading WordNet from {dict_dir}", file=sys.stderr)
    entries = build_entries(dict_dir)
    print(f"Parsed {len(entries)} unique words", file=sys.stderr)

    pages_idx, words_idx, defs_blob, n = pack_words(entries)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "pages.idx").write_bytes(pages_idx)
    (out / "words.idx").write_bytes(words_idx)
    (out / "defs.bin").write_bytes(defs_blob)

    print(f"\nWrote {out}/", file=sys.stderr)
    print(f"  pages.idx  {len(pages_idx):>10,} bytes  ({len(words_idx)//PAGE_SIZE} pages)", file=sys.stderr)
    print(f"  words.idx  {len(words_idx):>10,} bytes", file=sys.stderr)
    print(f"  defs.bin   {len(defs_blob):>10,} bytes", file=sys.stderr)
    print(f"  entries    {n:>10,}", file=sys.stderr)
    print(f"\nCopy to SD card at /shortbread/dict/", file=sys.stderr)


if __name__ == "__main__":
    main()
