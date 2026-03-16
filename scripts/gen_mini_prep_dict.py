#!/usr/bin/env python3
"""
Generate a small all-four-steps StarDict dictionary for per-book pre-processing tests.

All 4 preparation steps are required but complete in a few seconds on device.

Output: test/dictionaries/mini-prep/
  mini-prep.ifo       -- metadata
  mini-prep.dict.dz   -- compressed definitions (needs Extract dictionary step)
  mini-prep.idx       -- uncompressed word index (needs Generate dictionary offset file step)
  mini-prep.syn.dz    -- compressed synonyms (needs Extract synonyms + Generate synonym offset file steps)

Intentionally NOT written (device generates these during pre-processing):
  mini-prep.idx.oft
  mini-prep.syn.oft

Format: StarDict 2.4.2, sametypesequence=m (plain text definitions).
"""

import gzip
import io
import os
import struct

STEM = "mini-prep"

WORDS = [
    ("apple",    "A round fruit with red or green skin and a crisp white interior."),
    ("bench",    "A long seat, typically of wood or stone, for several people."),
    ("cloud",    "A visible mass of condensed water vapour floating in the atmosphere."),
    ("drift",    "To be carried slowly by a current of air or water."),
    ("ember",    "A small piece of burning or glowing coal or wood in a dying fire."),
    ("flint",    "A hard grey rock that produces sparks when struck against steel."),
    ("grove",    "A small group or stand of trees without undergrowth."),
    ("hinge",    "A jointed device on which a door, gate, or lid swings."),
    ("inlet",    "A small arm of the sea, a lake, or a river."),
    ("joust",    "To engage in a contest where two mounted knights charge with lances."),
    ("knoll",    "A small hill or mound."),
    ("latch",    "A metal bar that drops into a notch to fasten a gate or door."),
    ("maple",    "A tree or shrub with lobed leaves and winged seeds, valued for timber and syrup."),
    ("notch",    "A V-shaped cut in a surface or edge."),
    ("orbit",    "The curved path of a celestial body or spacecraft around a star or planet."),
    ("plank",    "A long flat piece of timber, used in building floors, decks, and fences."),
    ("quilt",    "A warm bed covering made of padding enclosed between layers of fabric."),
    ("ridge",    "A long narrow hilltop or mountain range."),
    ("shale",    "A fine-grained sedimentary rock formed from compressed mud or clay."),
    ("tidal",    "Relating to or affected by tides."),
    ("untie",    "To undo the tied parts of something; to loosen a knot."),
    ("valve",    "A device for controlling the passage of fluid through a pipe or duct."),
    ("wharf",    "A level quayside area to which a ship may be moored for loading."),
    ("xenon",    "A heavy inert gaseous element present in the atmosphere in trace amounts."),
    ("yield",    "To produce or provide; also to give way under pressure."),
    ("zonal",    "Relating to or divided into zones."),
]

# One synonym per word: word + "_syn" -> same ordinal
SYNONYMS = [(f"{w}_syn", i) for i, (w, _) in enumerate(WORDS)]


def main() -> None:
    script_dir = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.join(script_dir, "..", "test", "dictionaries", STEM)
    os.makedirs(out_dir, exist_ok=True)
    stem_path = os.path.join(out_dir, STEM)

    # Sort words lexicographically (already in order, but be explicit)
    sorted_words = sorted(WORDS, key=lambda x: x[0])

    # Build .idx and raw .dict
    dict_buf = io.BytesIO()
    idx_buf = io.BytesIO()

    for word, defn in sorted_words:
        defn_bytes = defn.encode("utf-8")
        off = dict_buf.tell()
        size = len(defn_bytes)
        dict_buf.write(defn_bytes)
        idx_buf.write(word.encode("ascii") + b"\x00")
        idx_buf.write(struct.pack(">II", off, size))

    dict_bytes = dict_buf.getvalue()
    idx_bytes = idx_buf.getvalue()

    # Build .syn (sorted lexicographically by synonym name)
    # Ordinal = position of canonical word in sorted_words
    word_to_ordinal = {w: i for i, (w, _) in enumerate(sorted_words)}
    sorted_syns = sorted(SYNONYMS, key=lambda x: x[0])

    syn_buf = io.BytesIO()
    for syn_word, word_idx in sorted_syns:
        canonical = sorted_words[word_idx][0]
        ordinal = word_to_ordinal[canonical]
        syn_buf.write(syn_word.encode("ascii") + b"\x00")
        syn_buf.write(struct.pack(">I", ordinal))
    syn_bytes = syn_buf.getvalue()

    # Write .dict.dz (compressed)
    with open(stem_path + ".dict.dz", "wb") as f:
        f.write(gzip.compress(dict_bytes, compresslevel=6))

    # Write .idx (uncompressed)
    with open(stem_path + ".idx", "wb") as f:
        f.write(idx_bytes)

    # Write .syn.dz (compressed)
    with open(stem_path + ".syn.dz", "wb") as f:
        f.write(gzip.compress(syn_bytes, compresslevel=6))

    # Write .ifo
    ifo_lines = [
        "StarDict's dict ifo file",
        "version=stardict-2.4.2",
        f"wordcount={len(sorted_words)}",
        f"idxfilesize={len(idx_bytes)}",
        f"synwordcount={len(sorted_syns)}",
        f"bookname=Mini Prep Test",
        "sametypesequence=m",
        "author=CrossPoint Test Suite",
        "description=Small synthetic dictionary for per-book pre-processing tests. All 4 steps required.",
    ]
    with open(stem_path + ".ifo", "w", encoding="utf-8") as f:
        f.write("\n".join(ifo_lines) + "\n")

    print(f"Files written to {out_dir}/")
    for ext in [".ifo", ".idx", ".dict.dz", ".syn.dz"]:
        path = stem_path + ext
        print(f"  {STEM}{ext:12s}  {os.path.getsize(path):6d} bytes")
    print()
    print("Note: .idx.oft and .syn.oft are intentionally absent so the device")
    print("runs all 4 pre-processing steps.")


if __name__ == "__main__":
    main()
