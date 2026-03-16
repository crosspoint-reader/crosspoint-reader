#!/usr/bin/env python3
"""
Generate a large all-prep StarDict dictionary for pre-processing timing tests.

Target: all 4 pre-processing steps take 5+ minutes combined on ESP32-C3.

Output: test/dictionaries/all-prep/
  all-prep.ifo       -- metadata
  all-prep.dict.dz   -- compressed definitions (~15-22 MB compressed, ~70+ MB raw)
  all-prep.idx       -- uncompressed word index (~2 MB)
  all-prep.syn.dz    -- compressed synonyms (~6-8 MB compressed, ~25 MB raw)

Intentionally NOT written (device generates these during pre-processing):
  all-prep.idx.oft
  all-prep.syn.oft

Format references (verified against real dicts):
  .ifo   : key=value text; version=stardict-2.4.2
  .idx   : [word\0][uint32 offset BE][uint32 size BE], sorted lexicographically
  .syn   : [synonym\0][uint32 idx_ordinal BE], sorted lexicographically
  Compressed files use standard gzip (magic 0x1F 0x8B).
  sametypesequence=m : plain text; size from .idx size field (no null terminator).
"""

import gzip
import io
import os
import struct
import sys
import time

# ---------------------------------------------------------------------------
# Tuning parameters
# ---------------------------------------------------------------------------

# Number of headwords. Each definition is ~900 bytes plain text.
# 100 000 words * ~880 bytes = ~84 MB raw .dict -> ~21 MB .dict.dz at ~4x gzip.
# Reduce if .dict.dz exceeds 25 MB; increase if total processing is too fast.
WORD_COUNT = 100_000

# Synonyms per headword. Each synonym entry is ~32 bytes in the .syn binary.
# Synonym names include a unique n*k derived suffix to resist gzip over-compression.
# 8 * 100 000 = 800 000 entries -> ~24 MB raw .syn -> ~7-9 MB .syn.dz at ~3x gzip.
SYNONYMS_PER_WORD = 8

STEM = "all-prep"


# ---------------------------------------------------------------------------
# Definition generator -- ~900 bytes of plain text, unique per entry
# ---------------------------------------------------------------------------

def make_definition(n: int) -> bytes:
    """
    Return ~900 bytes of plain-text definition for word number n.

    Content is structured to resist excessive gzip compression: shared
    structural phrases compress well, but each entry embeds n-derived
    numeric values that are unique and do not repeat across entries.
    """
    parts = [
        f"Entry {n:05d}.",
        f"This is test word number {n} in the CrossPoint pre-processing stress test dictionary.",
        f"Word all_prep_word_{n:05d} occupies ordinal {n - 1} in the sorted index.",
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


# ---------------------------------------------------------------------------
# StarDict binary helpers
# ---------------------------------------------------------------------------

OFT_HEADER = b"StarDict's Cache, Version: 0.2" + b"\xc1\xd1\xa4\x51\x00\x00\x00\x00"
assert len(OFT_HEADER) == 38
STRIDE = 32


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    script_dir = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.join(script_dir, "..", "test", "dictionaries", "all-prep")
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.join(out_dir, STEM)

    total_synonyms = WORD_COUNT * SYNONYMS_PER_WORD
    print(f"Generating {WORD_COUNT} headwords, {total_synonyms} synonyms "
          f"({SYNONYMS_PER_WORD} per word)...")

    t0 = time.monotonic()

    # ------------------------------------------------------------------
    # Build .idx and .dict in one pass
    # Headwords are all_prep_word_00001 ... all_prep_word_NNNNN
    # Zero-padded to 5 digits -> already in lexicographic order.
    # ------------------------------------------------------------------
    dict_buf = io.BytesIO()
    idx_buf = io.BytesIO()

    sample_len = len(make_definition(40000))
    print(f"Sample definition length (n=40000): {sample_len} bytes")

    for n in range(1, WORD_COUNT + 1):
        word = f"all_prep_word_{n:05d}"
        defn = make_definition(n)

        off = dict_buf.tell()
        size = len(defn)
        dict_buf.write(defn)

        idx_buf.write(word.encode("ascii") + b"\x00")
        idx_buf.write(struct.pack(">II", off, size))

        if n % 10000 == 0:
            elapsed = time.monotonic() - t0
            print(f"  words: {n}/{WORD_COUNT}  ({elapsed:.1f}s)")

    dict_bytes = dict_buf.getvalue()
    idx_bytes = idx_buf.getvalue()

    print(f"dict raw: {len(dict_bytes) / 1024 / 1024:.2f} MB  "
          f"idx: {len(idx_bytes) / 1024 / 1024:.2f} MB")

    # ------------------------------------------------------------------
    # Build .syn
    # Synonyms: all_prep_syn_NNNNN_v0 ... all_prep_syn_NNNNN_v7
    # each maps to canonical all_prep_word_NNNNN (ordinal = n-1).
    # Iterating n=1..80000, k=0..7 produces lexicographic order because
    # the 5-digit zero-padded n sorts before the next n, and v0<v1<...<v7.
    # ------------------------------------------------------------------
    syn_buf = io.BytesIO()

    for n in range(1, WORD_COUNT + 1):
        ordinal = n - 1  # 0-based position of all_prep_word_NNNNN in .idx
        for k in range(SYNONYMS_PER_WORD):
            # Include an n*k derived suffix to break gzip's pattern-matching and
            # keep the compressed .syn.dz size large enough to take time on device.
            suffix = (n * 31 + k * 97) % 10000
            syn_word = f"all_prep_syn_{n:05d}_v{k}_{suffix:04d}"
            syn_buf.write(syn_word.encode("ascii") + b"\x00")
            syn_buf.write(struct.pack(">I", ordinal))

        if n % 10000 == 0:
            elapsed = time.monotonic() - t0
            print(f"  synonyms: {n * SYNONYMS_PER_WORD}/{total_synonyms}  ({elapsed:.1f}s)")

    syn_bytes = syn_buf.getvalue()
    print(f"syn raw: {len(syn_bytes) / 1024 / 1024:.2f} MB")

    # ------------------------------------------------------------------
    # Compress and write .dict.dz
    # ------------------------------------------------------------------
    print("Compressing .dict.dz ...")
    t_compress = time.monotonic()
    dict_dz = gzip.compress(dict_bytes, compresslevel=6)
    del dict_bytes  # free RAM
    print(f"  done in {time.monotonic() - t_compress:.1f}s  "
          f"-> {len(dict_dz) / 1024 / 1024:.2f} MB")

    if len(dict_dz) > 25 * 1024 * 1024:
        print(f"WARNING: .dict.dz is {len(dict_dz)/1024/1024:.1f} MB, exceeds 25 MB target.",
              file=sys.stderr)

    with open(stem + ".dict.dz", "wb") as f:
        f.write(dict_dz)
    del dict_dz

    # ------------------------------------------------------------------
    # Compress and write .syn.dz
    # ------------------------------------------------------------------
    print("Compressing .syn.dz ...")
    t_compress = time.monotonic()
    syn_dz = gzip.compress(syn_bytes, compresslevel=6)
    del syn_bytes
    print(f"  done in {time.monotonic() - t_compress:.1f}s  "
          f"-> {len(syn_dz) / 1024 / 1024:.2f} MB")

    with open(stem + ".syn.dz", "wb") as f:
        f.write(syn_dz)
    del syn_dz

    # ------------------------------------------------------------------
    # Write .idx (uncompressed -- device must generate .idx.oft)
    # ------------------------------------------------------------------
    with open(stem + ".idx", "wb") as f:
        f.write(idx_bytes)

    # ------------------------------------------------------------------
    # Write .ifo
    # ------------------------------------------------------------------
    actual_syn_count = WORD_COUNT * SYNONYMS_PER_WORD
    ifo_lines = [
        "StarDict's dict ifo file",
        "version=stardict-2.4.2",
        f"wordcount={WORD_COUNT}",
        f"idxfilesize={len(idx_bytes)}",
        f"synwordcount={actual_syn_count}",
        "bookname=All Prep Test",
        "sametypesequence=m",
        "author=CrossPoint Test Suite",
        "description=Large synthetic dictionary for pre-processing timing tests on ESP32-C3.",
    ]
    with open(stem + ".ifo", "w", encoding="utf-8") as f:
        f.write("\n".join(ifo_lines) + "\n")

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    print(f"\nFiles written to {out_dir}/")
    for ext in [".ifo", ".dict.dz", ".idx", ".syn.dz"]:
        path = stem + ext
        size = os.path.getsize(path)
        mb = size / 1024 / 1024
        flag = "  *** OVER 25 MB ***" if mb > 25 else ""
        print(f"  {STEM}{ext:12s}  {mb:6.2f} MB{flag}")

    total_elapsed = time.monotonic() - t0
    print(f"\nTotal generation time: {total_elapsed:.1f}s")
    print(
        "\nNote: .idx.oft and .syn.oft are intentionally absent so the device "
        "runs all 4 pre-processing steps."
    )


if __name__ == "__main__":
    main()
