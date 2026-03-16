#!/usr/bin/env python3
"""
Generate a large all-four-steps StarDict dictionary for per-book cancel-prepare tests.

All 4 preparation steps are required and take 1-2 minutes combined on device --
long enough for the user to observe processing and press Back to cancel.

Output: test/dictionaries/long-prep/
  long-prep.ifo       -- metadata
  long-prep.dict.dz   -- compressed definitions (needs Extract dictionary step)
  long-prep.idx       -- uncompressed word index (needs Generate dictionary offset file step)
  long-prep.syn.dz    -- compressed synonyms (needs Extract synonyms + Generate synonym offset file steps)

Intentionally NOT written (device generates these during pre-processing):
  long-prep.idx.oft
  long-prep.syn.oft

Format: StarDict 2.4.2, sametypesequence=m (plain text definitions).
Modeled after gen_all_prep_dict.py at reduced scale for 1-2 minute processing time.
"""

import gzip
import io
import os
import struct
import sys
import time

STEM = "long-prep"

# ~25 000 headwords gives 1-2 minutes of processing on ESP32-C3.
# Each definition is ~200 bytes; 4 synonyms per word.
# Total idx: ~25000 * ~25 bytes = ~625 KB -> gen-idx step is fast but measurable.
# Total raw dict: ~25000 * ~200 bytes = ~5 MB -> extract-dict noticeable.
# Total raw syn: ~25000 * 4 * ~30 bytes = ~3 MB -> extract-syn + gen-syn noticeable.
WORD_COUNT = 25_000
SYNONYMS_PER_WORD = 4


def make_definition(n: int) -> bytes:
    parts = [
        f"Entry {n:05d}.",
        f"Test word long_prep_word_{n:05d} at ordinal {n - 1}.",
        f"Block A: {n*7+13} {n*11+17} {n*13+19} {n*17+23} {n*19+29} {n*23+31}.",
        f"Block B: {n*29+37} {n*31+41} {n*37+43} {n*41+47} {n*43+53} {n*47+59}.",
        f"Residue: m97={n%97} m89={n%89} m83={n%83} m79={n%79} m73={n%73}.",
        f"Seq: {n} {n+1} {n+2} {n+3} {n+5} {n+8} {n+13} {n+21} {n+34} {n+55}.",
        f"End of entry {n:05d}.",
    ]
    return " ".join(parts).encode("utf-8")


def main() -> None:
    script_dir = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.join(script_dir, "..", "test", "dictionaries", STEM)
    os.makedirs(out_dir, exist_ok=True)
    stem_path = os.path.join(out_dir, STEM)

    total_synonyms = WORD_COUNT * SYNONYMS_PER_WORD
    print(f"Generating {WORD_COUNT} headwords, {total_synonyms} synonyms...")

    t0 = time.monotonic()

    dict_buf = io.BytesIO()
    idx_buf = io.BytesIO()

    for n in range(1, WORD_COUNT + 1):
        word = f"long_prep_word_{n:05d}"
        defn = make_definition(n)
        off = dict_buf.tell()
        size = len(defn)
        dict_buf.write(defn)
        idx_buf.write(word.encode("ascii") + b"\x00")
        idx_buf.write(struct.pack(">II", off, size))
        if n % 5000 == 0:
            print(f"  words: {n}/{WORD_COUNT}  ({time.monotonic() - t0:.1f}s)")

    dict_bytes = dict_buf.getvalue()
    idx_bytes = idx_buf.getvalue()

    print(f"dict raw: {len(dict_bytes) / 1024:.0f} KB  idx: {len(idx_bytes) / 1024:.0f} KB")

    syn_buf = io.BytesIO()
    for n in range(1, WORD_COUNT + 1):
        ordinal = n - 1
        for k in range(SYNONYMS_PER_WORD):
            suffix = (n * 31 + k * 97) % 10000
            syn_word = f"long_prep_syn_{n:05d}_v{k}_{suffix:04d}"
            syn_buf.write(syn_word.encode("ascii") + b"\x00")
            syn_buf.write(struct.pack(">I", ordinal))
        if n % 5000 == 0:
            print(f"  synonyms: {n * SYNONYMS_PER_WORD}/{total_synonyms}  ({time.monotonic() - t0:.1f}s)")

    syn_bytes = syn_buf.getvalue()
    print(f"syn raw: {len(syn_bytes) / 1024:.0f} KB")

    print("Compressing .dict.dz ...")
    dict_dz = gzip.compress(dict_bytes, compresslevel=6)
    del dict_bytes
    print(f"  -> {len(dict_dz) / 1024:.0f} KB")

    with open(stem_path + ".dict.dz", "wb") as f:
        f.write(dict_dz)
    del dict_dz

    print("Compressing .syn.dz ...")
    syn_dz = gzip.compress(syn_bytes, compresslevel=6)
    del syn_bytes
    print(f"  -> {len(syn_dz) / 1024:.0f} KB")

    with open(stem_path + ".syn.dz", "wb") as f:
        f.write(syn_dz)
    del syn_dz

    with open(stem_path + ".idx", "wb") as f:
        f.write(idx_bytes)

    actual_syn_count = WORD_COUNT * SYNONYMS_PER_WORD
    ifo_lines = [
        "StarDict's dict ifo file",
        "version=stardict-2.4.2",
        f"wordcount={WORD_COUNT}",
        f"idxfilesize={len(idx_bytes)}",
        f"synwordcount={actual_syn_count}",
        "bookname=Long Prep Test",
        "sametypesequence=m",
        "author=CrossPoint Test Suite",
        "description=Medium synthetic dictionary for per-book cancel-prepare tests. All 4 steps required.",
    ]
    with open(stem_path + ".ifo", "w", encoding="utf-8") as f:
        f.write("\n".join(ifo_lines) + "\n")

    print(f"\nFiles written to {out_dir}/")
    for ext in [".ifo", ".dict.dz", ".idx", ".syn.dz"]:
        path = stem_path + ext
        mb = os.path.getsize(path) / 1024 / 1024
        print(f"  {STEM}{ext:12s}  {mb:.2f} MB")

    print(f"\nTotal generation time: {time.monotonic() - t0:.1f}s")
    print("\nNote: .idx.oft and .syn.oft are intentionally absent so the device")
    print("runs all 4 pre-processing steps.")


if __name__ == "__main__":
    main()
