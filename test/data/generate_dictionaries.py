#!/usr/bin/env python3
"""
generate_dictionaries.py — Unified StarDict dictionary generator.

Reads a JSON data file from test/data/dictionary-sources/ and produces all StarDict binary files.

Usage:
    python3 test/data/generate_dictionaries.py test/data/dictionary-sources/en-es.json   # one dict
    python3 test/data/generate_dictionaries.py --all                                     # all in test/data/dictionary-sources/

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

      generate_fpi:     bool — default for idx.fpi and syn.fpi generation (default false)
      generate_idx_fpi: bool — override idx.fpi generation (default: generate_fpi)
      generate_syn_fpi: bool — override syn.fpi generation (default: generate_fpi)
      generate_ifo:    bool — write .ifo file (default true)
      generate_idx:    bool — write .idx (default true)
      corrupt_dict:    bool — write invalid bytes as .dict.dz instead of real content
      base_entries:    name of another JSON in test/data/dictionary-sources/ to load entries from
      extra_ifo_files: list of {stem, bookname, ifo_version?} for extra .ifo files
      extra_idx_files: list of stem names for extra .idx file copies
    entries:           list of {headword, definition}  (omit when using base_entries)
    synonyms:          optional list of [synonym, canonical_headword]

  Synthetic (algorithmically generated):
    meta: { ... same as above (base_entries/extra_ifo_files not supported) ... }
    synthetic:
      word_prefix:       selects definition template
      syn_prefix:        synonym name prefix for sequential mode
      word_count:        int
      synonyms_per_word: int (default 0)
      word_style:        "sequential" (default) or "englishish"
      seed:              optional deterministic RNG seed

Format references:
  .ifo   : key=value text; version=stardict-2.4.2 (or 2.4.2)
  .idx   : [word\\0][uint32 offset BE][uint32 size BE], sorted lexicographically
  .syn   : [synonym\\0][uint32 idx_ordinal BE], sorted lexicographically
  sametypesequence=m : plain text; size from .idx (no null terminator)
  sametypesequence=h : HTML;       size from .idx (no null terminator)
"""

import argparse
import gzip
import io
import os
import random
import struct
import sys
import time
import zlib
from pathlib import Path
from typing import Optional

import json


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------



# Written as .dict.dz when corrupt_dict: true — starts with \x00\x00 (invalid gzip magic)
_CORRUPT_DICT_DATA = b"\x00\x00This is not a gzip file. Invalid magic bytes."

_ENGLISHISH_LETTER_POOL = (
    "eeeeeeeeeeeeeeeeeeee"
    "tttttttttttttttt"
    "aaaaaaaaaaaaaaa"
    "oooooooooooooo"
    "iiiiiiiiiiii"
    "nnnnnnnnnnnn"
    "sssssssssss"
    "rrrrrrrrrr"
    "hhhhhhhhh"
    "llllllll"
    "ddddddd"
    "cccccc"
    "uuuuuu"
    "mmmmmm"
    "wwwww"
    "fffff"
    "ggggg"
    "yyyyy"
    "pppp"
    "bbbb"
    "vvvv"
    "kkkk"
    "jj"
    "xx"
    "qq"
    "zz"
)

_ENGLISHISH_LENGTH_CDF = [
    (2, 2),
    (3, 8),
    (4, 19),
    (5, 34),
    (6, 50),
    (7, 65),
    (8, 77),
    (9, 87),
    (10, 93),
    (11, 97),
    (12, 99),
    (13, 100),
]

_DICTZIP_CHUNK_LEN = 58315


# ---------------------------------------------------------------------------
# StarDict binary helpers
# ---------------------------------------------------------------------------




# ---------------------------------------------------------------------------
# .fpi — the Fenced Prefix Index sidecar (tuning: fencep_6 + tab1_3), which minimizes
# SD sector reads for exact-match lookup; see the "Fenced Prefix Index" section of
# docs/dictionary-development.md for the motivation and the benchmark that picked
# this tuning. Supersedes .cspt for exact-match lookup. Mirrors
# Dictionary::generateFpi (src/util/Dictionary.cpp) and scripts/dictionary_tools.py's
# _build_fpi byte-for-byte — one of the three producers that must stay in sync.
# ---------------------------------------------------------------------------

_FPI_SECTOR_SIZE = 512
_FPI_VERSION = 0
_FPI_HEADER_SIZE = 1
_FPI_TAB_PREFIX_LEN = 3
_FPI_TAB_ENTRY_CAP = (_FPI_SECTOR_SIZE - _FPI_HEADER_SIZE) // _FPI_TAB_PREFIX_LEN  # 170
_FPI_FENCEP_PREFIX_LEN = 6
_FPI_GROUP_SECTORS = 8
_FPI_GROUP_ORDINAL_OFFSET = 9 + _FPI_GROUP_SECTORS * _FPI_FENCEP_PREFIX_LEN  # 57
_FPI_ORDINAL_SIZE = 4
_FPI_GROUP_SIZE = _FPI_GROUP_ORDINAL_OFFSET + _FPI_ORDINAL_SIZE  # 61
_FPI_GROUPS_PER_SIDECAR_SECTOR = _FPI_SECTOR_SIZE // _FPI_GROUP_SIZE  # 8
_FPI_MAX_COMMON_LEN = 127


def _fpi_sampled_sector(i: int, entry_count: int, sector_count: int) -> int:
    if entry_count <= 1 or sector_count <= 1:
        return 0
    return (i * (sector_count - 1)) // (entry_count - 1)


def build_fpi(src_bytes: bytes, skip_per_entry: int = 8) -> bytes:
    """Build .fpi bytes for .idx (skip=8) or .syn (skip=4).

    Single combined sidecar: sector 0 = a version byte + a "tab" table of up to
    170 3-byte lowercase prefixes, evenly sampled across source-file 512-byte
    sectors; sectors 1..N = a "fencep" sidecar of front-coded 6-byte prefixes, one
    per source-file sector, grouped 8 sectors at a time, plus a per-group 4-byte LE
    cumulative-ordinal field (the local==0 sector's fence word's 0-based entry
    ordinal, or the total entry count as a sentinel past the last entry) consumed
    by Dictionary::binarySearchFpiOrdinal.
    """
    import bisect

    src_len = len(src_bytes)
    sector_count = 0 if src_len == 0 else (src_len + _FPI_SECTOR_SIZE - 1) // _FPI_SECTOR_SIZE
    tab_entry_count = min(_FPI_TAB_ENTRY_CAP, sector_count)

    entries = []  # [(lowercased_word, entry_start), ...] in file order
    pos = 0
    while pos < src_len:
        null = src_bytes.index(b"\x00", pos)
        entries.append((src_bytes[pos:null].lower(), pos))
        pos = null + 1 + skip_per_entry
    starts = [e[1] for e in entries]

    fence_word = [b""] * sector_count
    fence_start = [0] * sector_count
    fence_ordinal = [len(entries)] * sector_count
    for sector in range(sector_count):
        i = bisect.bisect_left(starts, sector * _FPI_SECTOR_SIZE)
        if i < len(entries):
            fence_word[sector], fence_start[sector] = entries[i]
            fence_ordinal[sector] = i

    tab = bytearray(_FPI_SECTOR_SIZE - _FPI_HEADER_SIZE)
    for i in range(tab_entry_count):
        sector = _fpi_sampled_sector(i, tab_entry_count, sector_count)
        w = fence_word[sector][:_FPI_TAB_PREFIX_LEN]
        off = i * _FPI_TAB_PREFIX_LEN
        tab[off:off + len(w)] = w

    group_count = (sector_count + _FPI_GROUP_SECTORS - 1) // _FPI_GROUP_SECTORS
    fencep = bytearray()
    sidecar_buf = bytearray()
    groups_in_sector = 0
    for group in range(group_count):
        buf = bytearray(_FPI_GROUP_SIZE)
        prev = b""
        for local in range(_FPI_GROUP_SECTORS):
            sector = group * _FPI_GROUP_SECTORS + local
            if sector >= sector_count:
                break
            word = fence_word[sector]
            rel = 0
            if word:
                rel = (fence_start[sector] - sector * _FPI_SECTOR_SIZE) & 0x1ff
            buf[local] = rel & 0xff
            if rel & 0x100:
                buf[8] |= 1 << local
            if local == 0:
                buf[_FPI_GROUP_ORDINAL_OFFSET:_FPI_GROUP_ORDINAL_OFFSET + _FPI_ORDINAL_SIZE] = struct.pack(
                    "<I", fence_ordinal[sector])

            common = 0
            if local != 0:
                n = min(len(prev), len(word))
                while common < n and prev[common] == word[common]:
                    common += 1
                common = min(common, _FPI_MAX_COMMON_LEN)

            slot_off = 9 + local * _FPI_FENCEP_PREFIX_LEN
            if local != 0 and common > 0:
                suffix = word[common:common + _FPI_FENCEP_PREFIX_LEN - 1]
                suffix = suffix.ljust(_FPI_FENCEP_PREFIX_LEN - 1, b"\x00")
                buf[slot_off] = 0x80 | common
                buf[slot_off + 1:slot_off + _FPI_FENCEP_PREFIX_LEN] = suffix
                prev = prev[:common] + suffix
            else:
                pfx = word[:_FPI_FENCEP_PREFIX_LEN].ljust(_FPI_FENCEP_PREFIX_LEN, b"\x00")
                buf[slot_off:slot_off + _FPI_FENCEP_PREFIX_LEN] = pfx
                prev = pfx
        sidecar_buf += buf
        groups_in_sector += 1
        if groups_in_sector == _FPI_GROUPS_PER_SIDECAR_SECTOR:
            fencep += sidecar_buf.ljust(_FPI_SECTOR_SIZE, b"\x00")
            sidecar_buf = bytearray()
            groups_in_sector = 0
    if groups_in_sector > 0:
        fencep += sidecar_buf.ljust(_FPI_SECTOR_SIZE, b"\x00")

    return bytes([_FPI_VERSION]) + bytes(tab) + bytes(fencep)


def build_idx_fpi(idx_bytes: bytes) -> bytes:
    return build_fpi(idx_bytes, skip_per_entry=8)


def build_syn_fpi(syn_bytes: bytes) -> bytes:
    return build_fpi(syn_bytes, skip_per_entry=4)


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


def _build_dictzip(data: bytes, member_name: str, chunk_len: int = _DICTZIP_CHUNK_LEN) -> bytes:
    if chunk_len <= 0 or chunk_len > 0xFFFF:
        raise ValueError(f"invalid dictzip chunk_len {chunk_len}")

    chunk_payloads: list[bytes] = []
    chunk_sizes: list[int] = []
    for start in range(0, len(data), chunk_len):
        chunk = data[start:start + chunk_len]
        compressor = zlib.compressobj(level=6, wbits=-15)
        payload = compressor.compress(chunk) + compressor.flush()
        chunk_payloads.append(payload)
        chunk_sizes.append(len(payload))

    chunk_count = len(chunk_payloads)
    if chunk_count == 0:
        chunk_payloads.append(b"\x03\x00")
        chunk_sizes.append(2)
        chunk_count = 1

    ra_data = struct.pack("<HHH", 1, chunk_len, chunk_count)
    ra_data += struct.pack("<" + "H" * chunk_count, *chunk_sizes)
    extra = b"RA" + struct.pack("<H", len(ra_data)) + ra_data

    out = io.BytesIO()
    out.write(b"\x1f\x8b")          # ID1, ID2
    out.write(b"\x08")              # CM = deflate
    out.write(b"\x0c")              # FLG = FEXTRA | FNAME
    out.write(struct.pack("<I", 0)) # MTIME = 0 for stable output
    out.write(b"\x02")              # XFL = max compression
    out.write(b"\x03")              # OS = Unix
    out.write(struct.pack("<H", len(extra)))
    out.write(extra)
    out.write(member_name.encode("utf-8") + b"\x00")
    for payload in chunk_payloads:
        out.write(payload)
    out.write(struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF))
    out.write(struct.pack("<I", len(data) & 0xFFFFFFFF))
    return out.getvalue()


def write_or_compress(path: str, data: bytes, compress: bool) -> None:
    if compress:
        member_name = os.path.basename(path)
        with open(path + ".dz", "wb") as f:
            f.write(_build_dictzip(data, member_name))
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

    generate_idx_fpi = meta.get("generate_idx_fpi", meta.get("generate_fpi", False))
    generate_syn_fpi = meta.get("generate_syn_fpi", meta.get("generate_fpi", False))
    generate_ifo = meta.get("generate_ifo", True)
    generate_idx = meta.get("generate_idx", True)
    corrupt_dict = meta.get("corrupt_dict", False)
    extra_ifo_files = meta.get("extra_ifo_files", []) or []
    extra_idx_files = meta.get("extra_idx_files", []) or []

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

    # Write .idx and optionally .idx.oft.cspt / .idx.fpi
    if generate_idx:
        with open(stem + ".idx", "wb") as f:
            f.write(idx_bytes)

        if generate_idx_fpi:
            with open(stem + ".idx.fpi", "wb") as f:
                f.write(build_idx_fpi(idx_bytes))

    # Write .syn and optionally .syn.oft.cspt / .syn.fpi
    if syn_bytes:
        write_or_compress(stem + ".syn", syn_bytes, compress_syn)

        if generate_syn_fpi:
            with open(stem + ".syn.fpi", "wb") as f:
                f.write(build_syn_fpi(syn_bytes))

    # Write primary .ifo
    if generate_ifo:
        write_ifo(stem, meta, len(entries), len(idx_bytes), syn_count)

    # Write extra .ifo files (for multi-ifo test dict)
    for extra in extra_ifo_files:
        extra_stem = os.path.join(out_dir, extra["stem"])
        extra_meta = dict(meta)
        extra_meta["bookname"] = extra["bookname"]
        if "ifo_version" in extra:
            extra_meta["ifo_version"] = extra["ifo_version"]
        write_ifo(extra_stem, extra_meta, len(entries), len(idx_bytes), syn_count)

    # Write extra .idx copies (for multi-idx test dict)
    for extra_stem_name in extra_idx_files:
        extra_path = os.path.join(out_dir, extra_stem_name + ".idx")
        with open(extra_path, "wb") as f:
            f.write(idx_bytes)

    # Write extra arbitrary files (for macOS metadata test dict)
    extra_files = meta.get("extra_files", []) or []
    for ef in extra_files:
        file_path = os.path.join(out_dir, ef["name"])
        content = ef.get("content", "").encode("utf-8")
        with open(file_path, "wb") as f:
            f.write(content)

    # Summary
    exts = []
    if corrupt_dict:
        exts.append(".dict.dz")
    else:
        exts.append(".dict" + (".dz" if compress_dict else ""))
    if generate_idx:
        exts.append(".idx")
        if generate_idx_fpi:
            exts.append(".idx.fpi")
    if syn_bytes:
        exts.append(".syn" + (".dz" if compress_syn else ""))
        if generate_syn_fpi:
            exts.append(".syn.fpi")
    if generate_ifo:
        exts.append(".ifo")
    _print_summary(out_dir, stem_name, exts)
    for extra in extra_ifo_files:
        extra_path = os.path.join(out_dir, extra["stem"] + ".ifo")
        if os.path.exists(extra_path):
            size = os.path.getsize(extra_path)
            print(f"  {extra['stem']}.ifo{'':<16}  {size/1024/1024:7.3f} MB")
    for extra_stem_name in extra_idx_files:
        extra_path = os.path.join(out_dir, extra_stem_name + ".idx")
        if os.path.exists(extra_path):
            size = os.path.getsize(extra_path)
            print(f"  {extra_stem_name}.idx{'':<16}  {size/1024/1024:7.3f} MB")


# ---------------------------------------------------------------------------
# Synthetic definition templates
# ---------------------------------------------------------------------------

def _make_def_chain_stress(n: int, word_prefix: str, word_count: int,
                           headword: Optional[str] = None) -> bytes:
    """Large, style-dense HTML definition for the chained-OOM stress dict.

    Two stress properties on purpose:
      - ~23 KB of text / ~30 pages — worst-case for per-page layout memory.
      - 8 style transitions per line — maximises per-page styled-segment count
        (the allocation the text-pool must absorb without fragmenting).

    Embeds the NEXT headword (cyclic: word_count -> 1) as selectable plain text on
    every line, so a tester can chain deterministically and arbitrarily deep
    (also exercises the lookup-chain depth cap / history eviction)."""
    total = word_count if word_count > 0 else 1
    nxt = f"{word_prefix}_{(n % total) + 1:05d}"
    branch = f"{word_prefix}_{((n + 1) % total) + 1:05d}"
    tags = ["b", "i", "u", "code", "em", "strong"]
    # "** word **" (spaces INSIDE the markers) makes the headword a clean standalone
    # token for word-select (cleanWord strips the bare "**", keeps the word) AND an
    # easy visual anchor in the dense text.
    out = [f"<h1>Chained OOM stress entry {n:05d}</h1>",
           "<p>Large style-dense definition for layout-memory stress.</p>",
           f"<p>CHAIN &mdash; to continue the chain, look up ** {nxt} ** (it also appears on every page below).</p>",
           f"<p>BRANCH &mdash; for the back-then-forward test, look up ** {branch} ** instead.</p>"]
    for ln in range(120):
        words = []
        for w in range(8):
            tag = tags[(n + ln + w) % len(tags)]
            words.append(f"<{tag}>tok{ln:03d}{w}</{tag}>")
            words.append(f"plainword{ln:03d}{w}")
        words.insert(5, f"** {nxt} **")  # next headword: anchored + selectable, mid-line on every line
        out.append(f"<p>line {ln:03d} " + " ".join(words) + "</p>")
    return "".join(out).encode("utf-8")


def _make_def_all_prep(n: int, word_prefix: str, word_count: int = 0,
                       headword: Optional[str] = None) -> bytes:
    """~900-byte definition for all_prep_word dicts."""
    if headword is None:
        headword = f"{word_prefix}_{n:05d}"
    parts = [
        f"Entry {n:05d}.",
        f"This is test word number {n} in the CrossPoint pre-processing stress test dictionary.",
        f"Word {headword} occupies ordinal {n - 1} in the sorted index.",
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


def _make_def_long_prep(n: int, word_prefix: str, word_count: int = 0,
                        headword: Optional[str] = None) -> bytes:
    """~200-byte definition for long_prep_word dicts."""
    if headword is None:
        headword = f"{word_prefix}_{n:05d}"
    parts = [
        f"Entry {n:05d}.",
        f"Test word {headword} at ordinal {n - 1}.",
        f"Block A: {n*7+13} {n*11+17} {n*13+19} {n*17+23} {n*19+29} {n*23+31}.",
        f"Block B: {n*29+37} {n*31+41} {n*37+43} {n*41+47} {n*43+53} {n*47+59}.",
        f"Residue: m97={n%97} m89={n%89} m83={n%83} m79={n%79} m73={n%73}.",
        f"Seq: {n} {n+1} {n+2} {n+3} {n+5} {n+8} {n+13} {n+21} {n+34} {n+55}.",
        f"End of entry {n:05d}.",
    ]
    return " ".join(parts).encode("utf-8")


def _make_def_fuzzy(n: int, word_prefix: str, word_count: int = 0,
                    headword: Optional[str] = None) -> bytes:
    """Tiny definition for fuzzy_word dicts. Keeps the committed fixture small
    while still producing thousands of headwords (many .idx.fpi fence groups) so
    the .fpi-bracket binary search in Dictionary::findSimilar is exercised on a
    window that starts well past offset 0."""
    return f"def {n:05d}".encode("ascii")


_DEFINITION_FN = {
    "all_prep_word": _make_def_all_prep,
    "long_prep_word": _make_def_long_prep,
    "chain_stress": _make_def_chain_stress,
    "fuzzy_word": _make_def_fuzzy,
}


def _sample_englishish_length(rng: random.Random) -> int:
    roll = rng.randrange(100)
    for length, cutoff in _ENGLISHISH_LENGTH_CDF:
        if roll < cutoff:
            return length
    return _ENGLISHISH_LENGTH_CDF[-1][0]


def _make_englishish_word(rng: random.Random) -> str:
    length = _sample_englishish_length(rng)
    return "".join(
        _ENGLISHISH_LETTER_POOL[rng.randrange(len(_ENGLISHISH_LETTER_POOL))]
        for _ in range(length)
    )


def _generate_englishish_words(count: int, rng: random.Random,
                               used: Optional[set[str]] = None) -> list[str]:
    if used is None:
        used = set()
    words: list[str] = []
    while len(words) < count:
        word = _make_englishish_word(rng)
        if word in used:
            continue
        used.add(word)
        words.append(word)
    words.sort()
    return words


# ---------------------------------------------------------------------------
# Synthetic builder
# ---------------------------------------------------------------------------

def build_synthetic(cfg: dict, out_dir: str) -> None:
    meta = cfg["meta"]
    syn_cfg = cfg["synthetic"]
    stem_name = meta["name"]
    compress = meta.get("compress", False)

    word_prefix = syn_cfg["word_prefix"]
    syn_prefix = syn_cfg["syn_prefix"]
    word_count = syn_cfg["word_count"]
    synonyms_per_word = syn_cfg.get("synonyms_per_word", 0)
    word_style = syn_cfg.get("word_style", "sequential")

    if word_prefix not in _DEFINITION_FN:
        print(f"ERROR: unknown word_prefix '{word_prefix}'. "
              f"Known: {list(_DEFINITION_FN)}", file=sys.stderr)
        sys.exit(1)
    make_def = _DEFINITION_FN[word_prefix]

    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.join(out_dir, stem_name)
    rng = random.Random(syn_cfg.get("seed", f"{stem_name}:{word_count}:{synonyms_per_word}:{word_style}"))

    total_synonyms = word_count * synonyms_per_word
    print(f"Generating {word_count:,} headwords, {total_synonyms:,} synonyms...")

    t0 = time.monotonic()

    if word_style == "englishish":
        headwords = _generate_englishish_words(word_count, rng)
    elif word_style == "sequential":
        headwords = [f"{word_prefix}_{n:05d}" for n in range(1, word_count + 1)]
    else:
        print(f"ERROR: unknown word_style '{word_style}'. Known: sequential, englishish",
              file=sys.stderr)
        sys.exit(1)

    entries = []
    for n, word in enumerate(headwords, start=1):
        entries.append((word, make_def(n, word_prefix, word_count, word).decode("utf-8")))
        if n % 10000 == 0:
            print(f"  words: {n:,}/{word_count:,}  ({time.monotonic() - t0:.1f}s)")

    idx_bytes, dict_bytes = build_idx_dict(entries)
    print(f"dict raw: {len(dict_bytes) / 1024 / 1024:.2f} MB  "
          f"idx: {len(idx_bytes) / 1024 / 1024:.2f} MB")

    syn_bytes = b""
    if synonyms_per_word > 0:
        headword_ordinals = {word: ordinal for ordinal, word in enumerate(headwords)}
        synonym_pairs = []
        used_synonyms = set(headwords)
        for n, word in enumerate(headwords, start=1):
            for k in range(synonyms_per_word):
                if word_style == "englishish":
                    syn_word = _generate_englishish_words(1, rng, used_synonyms)[0]
                else:
                    suffix = (n * 31 + k * 97) % 10000
                    syn_word = f"{syn_prefix}_{n:05d}_v{k}_{suffix:04d}"
                synonym_pairs.append((syn_word, word))
            if n % 10000 == 0:
                elapsed = time.monotonic() - t0
                print(f"  synonyms: {n * synonyms_per_word:,}/{total_synonyms:,}"
                      f"  ({elapsed:.1f}s)")
        synonym_pairs.sort(key=lambda item: item[0])
        syn_bytes, _ = build_syn(synonym_pairs, headword_ordinals)
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

    # Write .idx (always uncompressed) and the CrossPoint
    # accelerator index (.idx.fpi).
    with open(stem + ".idx", "wb") as f:
        f.write(idx_bytes)
    generate_idx_fpi = meta.get("generate_idx_fpi", meta.get("generate_fpi", False))
    if generate_idx_fpi:
        with open(stem + ".idx.fpi", "wb") as f:
            f.write(build_idx_fpi(idx_bytes))

    # Write .syn / .syn.dz (+ optional .syn.oft.cspt / .syn.fpi)
    syn_count = None
    generate_syn_fpi = meta.get("generate_syn_fpi", meta.get("generate_fpi", False))
    if syn_bytes:
        print("Compressing .syn.dz ..." if compress else "Writing .syn ...")
        write_or_compress(stem + ".syn", syn_bytes, compress)
        if generate_syn_fpi:
            with open(stem + ".syn.fpi", "wb") as f:
                f.write(build_syn_fpi(syn_bytes))
        del syn_bytes
        syn_count = total_synonyms

    write_ifo(stem, meta, word_count, len(idx_bytes), syn_count)

    exts = [".ifo",
            ".dict" + (".dz" if compress else ""),
            ".idx",
            ".idx.fpi" if generate_idx_fpi else None,
            (".syn" + (".dz" if compress else "")) if syn_count else None,
            ".syn.fpi" if (generate_syn_fpi and syn_count) else None]
    _print_summary(out_dir, stem_name, exts)
    print(f"\nTotal generation time: {time.monotonic() - t0:.1f}s")


# ---------------------------------------------------------------------------
# Router
# ---------------------------------------------------------------------------

def generate(yaml_path: str) -> None:
    with open(yaml_path, encoding="utf-8") as f:
        cfg = json.load(f)

    meta = cfg["meta"]
    yaml_dir = os.path.dirname(os.path.abspath(yaml_path))
    script_dir = os.path.dirname(os.path.abspath(__file__))
    workspace_dir = os.path.dirname(os.path.dirname(script_dir))
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
        description="Generate StarDict dictionaries from JSON data files in test/data/dictionary-sources/."
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("json_file", nargs="?",
                       help="Path to a single JSON data file")
    group.add_argument("--all", action="store_true",
                       help="Generate all *.json files in test/data/dictionary-sources/")
    args = parser.parse_args()

    if args.all:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        sources_dir = os.path.join(script_dir, "dictionary-sources")
        yaml_files = sorted(Path(sources_dir).glob("*.json"))
        if not yaml_files:
            print(f"No JSON files found in {sources_dir}", file=sys.stderr)
            sys.exit(1)
        for yf in yaml_files:
            generate(str(yf))
    else:
        generate(args.json_file)


if __name__ == "__main__":
    main()
