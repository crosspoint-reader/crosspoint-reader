#!/usr/bin/env python3
"""
gen_dict.py — Unified StarDict dictionary generator.

Reads a YAML data file from scripts/dicts/ and produces all StarDict binary files.

Usage:
    python3 scripts/gen_dict.py scripts/dicts/en_es.yaml        # one dict
    python3 scripts/gen_dict.py --all                            # all dicts in scripts/dicts/

YAML schemas:

  Data-driven (entries list or base_entries reference):
    meta:
      name:            stem name for output files (may contain hyphens)
      bookname:        displayed name
      output_dir:      path relative to workspace root
      entry_format:    m (plain text) or h (HTML)
      ifo_version:     default "stardict-2.4.2"; use "2.4.2" for older format
      author:          optional
      description:     optional
      compress:        bool — default for dict and syn compression (default false)
      compress_dict:   bool — override dict compression (default: compress)
      compress_syn:    bool — override syn compression (default: compress)
      generate_oft:    bool — default for idx.oft and syn.oft generation (default false)
      generate_idx_oft: bool — override idx.oft generation (default: generate_oft)
      generate_syn_oft: bool — override syn.oft generation (default: generate_oft)
      generate_ifo:    bool — write .ifo file (default true)
      generate_idx:    bool — write .idx (and .idx.oft) (default true)
      corrupt_dict:    bool — write invalid bytes as .dict.dz instead of real content
      base_entries:    name of another YAML in scripts/dicts/ to load entries from
      extra_ifo_files: list of {stem, bookname, ifo_version?} for extra .ifo files
    entries:           list of {headword, definition}  (omit when using base_entries)
    synonyms:          optional list of [synonym, canonical_headword]

  Synthetic (algorithmically generated):
    meta: { ... same as above (base_entries/extra_ifo_files not supported) ... }
    synthetic:
      word_prefix:       selects definition template and word name format
      syn_prefix:        synonym name prefix
      word_count:        int
      synonyms_per_word: int (default 0)

Format references:
  .ifo   : key=value text; version=stardict-2.4.2 (or 2.4.2)
  .idx   : [word\\0][uint32 offset BE][uint32 size BE], sorted lexicographically
  .syn   : [synonym\\0][uint32 idx_ordinal BE], sorted lexicographically
  .oft   : 38-byte header + LE uint32 offsets at stride=32, plus sentinel
           header = b"StarDict's Cache, Version: 0.2" (30 bytes)
                  + b"\\xc1\\xd1\\xa4\\x51\\x00\\x00\\x00\\x00" (8 bytes)
           offset[0]   = byte offset of entry 32 in .idx (or .syn)
           offset[1]   = byte offset of entry 64
           ...
           offset[N-1] = byte offset of entry N*32
           offset[N]   = total byte size of the .idx (or .syn) file  ← sentinel
  sametypesequence=m : plain text; size from .idx (no null terminator)
  sametypesequence=h : HTML;       size from .idx (no null terminator)
"""

import argparse
import gzip
import io
import os
import struct
import sys
import time
from pathlib import Path

import json


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

OFT_HEADER = b"StarDict's Cache, Version: 0.2" + b"\xc1\xd1\xa4\x51\x00\x00\x00\x00"
assert len(OFT_HEADER) == 38
STRIDE = 32

# Written as .dict.dz when corrupt_dict: true — starts with \x00\x00 (invalid gzip magic)
_CORRUPT_DICT_DATA = b"\x00\x00This is not a gzip file. Invalid magic bytes."


# ---------------------------------------------------------------------------
# StarDict binary helpers
# ---------------------------------------------------------------------------

def _build_oft(binary: bytes, skip_bytes_after_null: int) -> bytes:
    """Build .oft bytes for idx (skip_bytes=8) or syn (skip_bytes=4)."""
    offsets = []
    entry_count = 0
    pos = 0
    while pos < len(binary):
        null = binary.index(b"\x00", pos)
        pos = null + 1 + skip_bytes_after_null
        entry_count += 1
        if entry_count % STRIDE == 0:
            offsets.append(pos)
    offsets.append(len(binary))  # sentinel: total byte size of the source file
    data = OFT_HEADER
    for off in offsets:
        data += struct.pack("<I", off)
    return data


def build_idx_oft(idx_bytes: bytes) -> bytes:
    return _build_oft(idx_bytes, skip_bytes_after_null=8)


def build_syn_oft(syn_bytes: bytes) -> bytes:
    return _build_oft(syn_bytes, skip_bytes_after_null=4)


def build_idx_dict(entries: list) -> tuple:
    """
    Build .idx and .dict binaries.
    entries: sorted list of (headword: str, definition: str).
    Returns (idx_bytes, dict_bytes).
    """
    idx = io.BytesIO()
    dct = io.BytesIO()
    for word, defn in entries:
        defn_b = defn.encode("utf-8")
        off = dct.tell()
        size = len(defn_b)
        dct.write(defn_b)
        idx.write(word.encode("utf-8") + b"\x00")
        idx.write(struct.pack(">II", off, size))
    return idx.getvalue(), dct.getvalue()


def build_syn(synonym_pairs: list, headword_ordinals: dict) -> tuple:
    """
    Build .syn binary.
    synonym_pairs: sorted list of (synonym_str, canonical_headword_str).
    headword_ordinals: {headword: 0-based ordinal in .idx}.
    Returns (syn_bytes, valid_count). Skips synonyms whose canonical is absent.
    """
    buf = io.BytesIO()
    valid_count = 0
    for syn_word, canonical in synonym_pairs:
        if canonical not in headword_ordinals:
            print(f"  SKIP synonym '{syn_word}' → '{canonical}' (not in index)")
            continue
        buf.write(syn_word.encode("utf-8") + b"\x00")
        buf.write(struct.pack(">I", headword_ordinals[canonical]))
        valid_count += 1
    return buf.getvalue(), valid_count


def write_or_compress(path: str, data: bytes, compress: bool) -> None:
    if compress:
        with open(path + ".dz", "wb") as f:
            f.write(gzip.compress(data, compresslevel=6))
    else:
        with open(path, "wb") as f:
            f.write(data)


def write_ifo(stem: str, meta: dict, wordcount: int, idxfilesize: int,
              synwordcount=None) -> None:
    version = meta.get("ifo_version", "stardict-2.4.2")
    lines = [
        "StarDict's dict ifo file",
        f"version={version}",
        f"wordcount={wordcount}",
    ]
    if synwordcount is not None:
        lines.append(f"synwordcount={synwordcount}")
    lines.append(f"idxfilesize={idxfilesize}")
    lines.append(f"bookname={meta['bookname']}")
    lines.append(f"sametypesequence={meta['entry_format']}")
    if "author" in meta:
        lines.append(f"author={meta['author']}")
    if "description" in meta:
        lines.append(f"description={meta['description']}")
    if "website" in meta:
        lines.append(f"website={meta['website']}")
    if "date" in meta:
        lines.append(f"date={meta['date']}")
    if "lang" in meta:
        lines.append(f"lang={meta['lang']}")
    with open(stem + ".ifo", "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def _print_summary(out_dir: str, stem_name: str, extensions: list) -> None:
    print(f"\nFiles written to {out_dir}/")
    for ext in extensions:
        if ext is None:
            continue
        path = os.path.join(out_dir, stem_name + ext)
        if not os.path.exists(path):
            continue
        size = os.path.getsize(path)
        mb = size / 1024 / 1024
        print(f"  {stem_name}{ext:20s}  {mb:7.3f} MB")


# ---------------------------------------------------------------------------
# Data-driven builder
# ---------------------------------------------------------------------------

def _load_entries_from_yaml(yaml_path: str) -> list:
    with open(yaml_path, encoding="utf-8") as f:
        base_cfg = json.load(f)
    return [(str(e["headword"]), str(e["definition"])) for e in base_cfg["entries"]]


def build_data_driven(cfg: dict, out_dir: str, yaml_dir: str) -> None:
    meta = cfg["meta"]
    stem_name = meta["name"]

    # Compression and generation flags
    compress_dict = meta.get("compress_dict", meta.get("compress", False))
    compress_syn = meta.get("compress_syn", meta.get("compress", False))
    generate_idx_oft = meta.get("generate_idx_oft", meta.get("generate_oft", False))
    generate_syn_oft = meta.get("generate_syn_oft", meta.get("generate_oft", False))
    generate_ifo = meta.get("generate_ifo", True)
    generate_idx = meta.get("generate_idx", True)
    corrupt_dict = meta.get("corrupt_dict", False)
    extra_ifo_files = meta.get("extra_ifo_files", []) or []

    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.join(out_dir, stem_name)

    # Load entries (from base_entries or directly)
    if "base_entries" in meta:
        base_yaml = os.path.join(yaml_dir, meta["base_entries"] + ".json")
        raw = _load_entries_from_yaml(base_yaml)
    else:
        raw = [(str(e["headword"]), str(e["definition"])) for e in cfg["entries"]]

    raw.sort(key=lambda e: e[0])
    seen: set = set()
    entries = []
    for hw, defn in raw:
        if hw not in seen:
            seen.add(hw)
            entries.append((hw, defn))

    headword_ordinals = {hw: i for i, (hw, _) in enumerate(entries)}
    print(f"Headwords: {len(entries)}")

    # Load synonyms
    syn_pairs = [(pair[0], pair[1]) for pair in cfg.get("synonyms", [])]
    syn_pairs.sort(key=lambda x: x[0])
    seen_syn: set = set()
    unique_syn = []
    for pair in syn_pairs:
        if pair[0] not in seen_syn:
            seen_syn.add(pair[0])
            unique_syn.append(pair)
    syn_pairs = unique_syn

    # Build binary data (always build idx_bytes for ifo idxfilesize)
    idx_bytes, dict_bytes = build_idx_dict(entries)

    syn_bytes = b""
    syn_count = None
    if syn_pairs:
        syn_bytes, syn_count = build_syn(syn_pairs, headword_ordinals)
        print(f"Synonyms:  {syn_count}")

    # Write .dict (or corrupt .dict.dz)
    if corrupt_dict:
        with open(stem + ".dict.dz", "wb") as f:
            f.write(_CORRUPT_DICT_DATA)
    else:
        write_or_compress(stem + ".dict", dict_bytes, compress_dict)

    # Write .idx and optionally .idx.oft
    if generate_idx:
        with open(stem + ".idx", "wb") as f:
            f.write(idx_bytes)
        if generate_idx_oft:
            with open(stem + ".idx.oft", "wb") as f:
                f.write(build_idx_oft(idx_bytes))

    # Write .syn and optionally .syn.oft
    if syn_bytes:
        write_or_compress(stem + ".syn", syn_bytes, compress_syn)
        if generate_syn_oft:
            with open(stem + ".syn.oft", "wb") as f:
                f.write(build_syn_oft(syn_bytes))

    # Write primary .ifo
    if generate_ifo:
        write_ifo(stem, meta, len(entries), len(idx_bytes), syn_count)

    # Write extra .ifo files (for multi-ifo dicts)
    for extra in extra_ifo_files:
        extra_stem = os.path.join(out_dir, extra["stem"])
        extra_meta = dict(meta)
        extra_meta["bookname"] = extra["bookname"]
        if "ifo_version" in extra:
            extra_meta["ifo_version"] = extra["ifo_version"]
        write_ifo(extra_stem, extra_meta, len(entries), len(idx_bytes), syn_count)

    # Summary
    exts = []
    if corrupt_dict:
        exts.append(".dict.dz")
    else:
        exts.append(".dict" + (".dz" if compress_dict else ""))
    if generate_idx:
        exts.append(".idx")
        if generate_idx_oft:
            exts.append(".idx.oft")
    if syn_bytes:
        exts.append(".syn" + (".dz" if compress_syn else ""))
        if generate_syn_oft:
            exts.append(".syn.oft")
    if generate_ifo:
        exts.append(".ifo")
    _print_summary(out_dir, stem_name, exts)
    for extra in extra_ifo_files:
        extra_path = os.path.join(out_dir, extra["stem"] + ".ifo")
        if os.path.exists(extra_path):
            size = os.path.getsize(extra_path)
            print(f"  {extra['stem']}.ifo{'':<16}  {size/1024/1024:7.3f} MB")


# ---------------------------------------------------------------------------
# Synthetic definition templates
# ---------------------------------------------------------------------------

def _make_def_all_prep(n: int, word_prefix: str) -> bytes:
    """~900-byte definition for all_prep_word dicts."""
    parts = [
        f"Entry {n:05d}.",
        f"This is test word number {n} in the CrossPoint pre-processing stress test dictionary.",
        f"Word {word_prefix}_{n:05d} occupies ordinal {n - 1} in the sorted index.",
        f"Arithmetic block A: {n*7+13} {n*11+17} {n*13+19} {n*17+23} {n*19+29} {n*23+31}.",
        f"Arithmetic block B: {n*29+37} {n*31+41} {n*37+43} {n*41+47} {n*43+53} {n*47+59}.",
        f"Arithmetic block C: {n*53+61} {n*59+67} {n*61+71} {n*67+73} {n*71+79} {n*73+83}.",
        f"Residue block: m97={n%97} m89={n%89} m83={n%83} m79={n%79} m73={n%73} m71={n%71} m67={n%67} m61={n%61}.",
        f"Hash block: h1={n*101+7} h2={n*103+11} h3={n*107+13} h4={n*109+17} h5={n*113+19} h6={n*127+23}.",
        f"Index data: page={n // 32} slot={n % 32} stride32_off={n * 32} seq={n * (n % 7 + 1)}.",
        f"Extended ref: s7={n*7} s11={n*11} s13={n*13} s17={n*17} s19={n*19} s23={n*23} s29={n*29}.",
        f"Sequence: {n} {n+1} {n+2} {n+3} {n+5} {n+8} {n+13} {n+21} {n+34} {n+55} {n+89} {n+144} {n+233}.",
        f"Padding: {n*997%10000:04d} {n*991%10000:04d} {n*983%10000:04d} {n*977%10000:04d}"
        f" {n*971%10000:04d} {n*967%10000:04d} {n*953%10000:04d} {n*947%10000:04d}.",
        f"Verification: xor={n ^ (n >> 8)} sum={n + (n >> 4) + (n >> 8) + (n >> 12)} inv={99999 - n}.",
        f"End of entry {n:05d}.",
    ]
    return " ".join(parts).encode("utf-8")


def _make_def_long_prep(n: int, word_prefix: str) -> bytes:
    """~200-byte definition for long_prep_word dicts."""
    parts = [
        f"Entry {n:05d}.",
        f"Test word {word_prefix}_{n:05d} at ordinal {n - 1}.",
        f"Block A: {n*7+13} {n*11+17} {n*13+19} {n*17+23} {n*19+29} {n*23+31}.",
        f"Block B: {n*29+37} {n*31+41} {n*37+43} {n*41+47} {n*43+53} {n*47+59}.",
        f"Residue: m97={n%97} m89={n%89} m83={n%83} m79={n%79} m73={n%73}.",
        f"Seq: {n} {n+1} {n+2} {n+3} {n+5} {n+8} {n+13} {n+21} {n+34} {n+55}.",
        f"End of entry {n:05d}.",
    ]
    return " ".join(parts).encode("utf-8")


_DEFINITION_FN = {
    "all_prep_word": _make_def_all_prep,
    "long_prep_word": _make_def_long_prep,
}


# ---------------------------------------------------------------------------
# Synthetic builder
# ---------------------------------------------------------------------------

def build_synthetic(cfg: dict, out_dir: str) -> None:
    meta = cfg["meta"]
    syn_cfg = cfg["synthetic"]
    stem_name = meta["name"]
    compress = meta.get("compress", False)
    generate_oft = meta.get("generate_oft", False)

    word_prefix = syn_cfg["word_prefix"]
    syn_prefix = syn_cfg["syn_prefix"]
    word_count = syn_cfg["word_count"]
    synonyms_per_word = syn_cfg.get("synonyms_per_word", 0)

    if word_prefix not in _DEFINITION_FN:
        print(f"ERROR: unknown word_prefix '{word_prefix}'. "
              f"Known: {list(_DEFINITION_FN)}", file=sys.stderr)
        sys.exit(1)
    make_def = _DEFINITION_FN[word_prefix]

    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.join(out_dir, stem_name)

    total_synonyms = word_count * synonyms_per_word
    print(f"Generating {word_count:,} headwords, {total_synonyms:,} synonyms...")

    t0 = time.monotonic()

    dict_buf = io.BytesIO()
    idx_buf = io.BytesIO()

    for n in range(1, word_count + 1):
        word = f"{word_prefix}_{n:05d}"
        defn = make_def(n, word_prefix)
        off = dict_buf.tell()
        size = len(defn)
        dict_buf.write(defn)
        idx_buf.write(word.encode("ascii") + b"\x00")
        idx_buf.write(struct.pack(">II", off, size))
        if n % 10000 == 0:
            print(f"  words: {n:,}/{word_count:,}  ({time.monotonic() - t0:.1f}s)")

    dict_bytes = dict_buf.getvalue()
    idx_bytes = idx_buf.getvalue()
    print(f"dict raw: {len(dict_bytes) / 1024 / 1024:.2f} MB  "
          f"idx: {len(idx_bytes) / 1024 / 1024:.2f} MB")

    syn_bytes = b""
    if synonyms_per_word > 0:
        syn_buf = io.BytesIO()
        for n in range(1, word_count + 1):
            ordinal = n - 1
            for k in range(synonyms_per_word):
                suffix = (n * 31 + k * 97) % 10000
                syn_word = f"{syn_prefix}_{n:05d}_v{k}_{suffix:04d}"
                syn_buf.write(syn_word.encode("ascii") + b"\x00")
                syn_buf.write(struct.pack(">I", ordinal))
            if n % 10000 == 0:
                elapsed = time.monotonic() - t0
                print(f"  synonyms: {n * synonyms_per_word:,}/{total_synonyms:,}"
                      f"  ({elapsed:.1f}s)")
        syn_bytes = syn_buf.getvalue()
        print(f"syn raw: {len(syn_bytes) / 1024 / 1024:.2f} MB")

    # Write .dict / .dict.dz
    print("Compressing .dict.dz ..." if compress else "Writing .dict ...")
    write_or_compress(stem + ".dict", dict_bytes, compress)
    if compress and len(gzip.compress(b"")) > 0:
        # Check size of actual file written
        dz_path = stem + ".dict.dz"
        if os.path.exists(dz_path):
            dz_size = os.path.getsize(dz_path)
            if dz_size > 25 * 1024 * 1024:
                print(f"WARNING: .dict.dz is {dz_size/1024/1024:.1f} MB, exceeds 25 MB target.",
                      file=sys.stderr)
    del dict_bytes

    # Write .idx (always uncompressed)
    with open(stem + ".idx", "wb") as f:
        f.write(idx_bytes)

    # Write .syn / .syn.dz
    syn_count = None
    if syn_bytes:
        print("Compressing .syn.dz ..." if compress else "Writing .syn ...")
        write_or_compress(stem + ".syn", syn_bytes, compress)
        del syn_bytes
        syn_count = total_synonyms

    write_ifo(stem, meta, word_count, len(idx_bytes), syn_count)

    exts = [".ifo",
            ".dict" + (".dz" if compress else ""),
            ".idx",
            (".syn" + (".dz" if compress else "")) if syn_count else None]
    _print_summary(out_dir, stem_name, exts)
    print(f"\nTotal generation time: {time.monotonic() - t0:.1f}s")
    if not generate_oft:
        print("Note: .idx.oft and .syn.oft intentionally absent — device generates them.")


# ---------------------------------------------------------------------------
# Router
# ---------------------------------------------------------------------------

def generate(yaml_path: str) -> None:
    with open(yaml_path, encoding="utf-8") as f:
        cfg = json.load(f)

    meta = cfg["meta"]
    yaml_dir = os.path.dirname(os.path.abspath(yaml_path))
    script_dir = os.path.dirname(os.path.abspath(__file__))
    workspace_dir = os.path.dirname(script_dir)
    out_dir = os.path.join(workspace_dir, meta["output_dir"])

    print(f"\n{'='*60}")
    print(f"Generating '{meta['bookname']}' → {out_dir}")
    print(f"{'='*60}")

    if "synthetic" in cfg:
        build_synthetic(cfg, out_dir)
    elif "entries" in cfg or "base_entries" in meta:
        build_data_driven(cfg, out_dir, yaml_dir)
    else:
        print(f"ERROR: {yaml_path} has neither 'entries', 'base_entries', nor 'synthetic' block",
              file=sys.stderr)
        sys.exit(1)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate StarDict dictionaries from YAML data files in scripts/dicts/."
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("yaml_file", nargs="?",
                       help="Path to a single YAML data file")
    group.add_argument("--all", action="store_true",
                       help="Generate all *.yaml files in scripts/dicts/")
    args = parser.parse_args()

    if args.all:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        dicts_dir = os.path.join(script_dir, "dicts")
        yaml_files = sorted(Path(dicts_dir).glob("*.json"))
        if not yaml_files:
            print(f"No YAML files found in {dicts_dir}", file=sys.stderr)
            sys.exit(1)
        for yf in yaml_files:
            generate(str(yf))
    else:
        generate(args.yaml_file)


if __name__ == "__main__":
    main()
