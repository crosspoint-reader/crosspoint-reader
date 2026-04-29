#!/usr/bin/env python3
"""
dict_tools.py — Offline StarDict dictionary tools for CrossPoint Reader.

Subcommands:
  prep   — Pre-process a dictionary (decompress, generate offset files)
  lookup — Look up a word in a prepared dictionary
  merge  — Merge multiple StarDict dictionaries into one

Run 'python3 scripts/dictionary_tools.py <subcommand> --help' for details.
"""

import argparse
import gzip
import shutil
import struct
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# .oft constants — matches StarDict cache format and DictPrepareActivity
# ---------------------------------------------------------------------------

_OFT_HEADER = b"StarDict's Cache, Version: 0.2" + b"\xc1\xd1\xa4\x51\x00\x00\x00\x00"
assert len(_OFT_HEADER) == 38
_STRIDE = 32


def _build_oft(data: bytes, skip_bytes_after_null: int) -> bytes:
    """Build .oft index bytes for .idx (skip=8) or .syn (skip=4)."""
    offsets = []
    entry_count = 0
    pos = 0
    while pos < len(data):
        null = data.index(b"\x00", pos)
        pos = null + 1 + skip_bytes_after_null
        entry_count += 1
        if entry_count % _STRIDE == 0:
            offsets.append(pos)
    offsets.append(len(data))  # sentinel: total byte size of the source file
    result = _OFT_HEADER
    for off in offsets:
        result += struct.pack("<I", off)
    return result


# ---------------------------------------------------------------------------
# .cspt constants — CrossPoint optimized index
# ---------------------------------------------------------------------------

_CSPT_MAGIC = b"CSPT"
_CSPT_VERSION = 1
_CSPT_PREFIX_LEN = 16
_CSPT_STRIDE = 16
_CSPT_HEADER_SIZE = 12
_CSPT_ENTRY_SIZE = _CSPT_PREFIX_LEN + 4  # 20 bytes


def _build_cspt(idx_data: bytes, oft_data: bytes) -> bytes:
    """Build .idx.oft.cspt from .idx data and .idx.oft data."""
    # Parse OFT: header (38 bytes) + LE uint32 page offsets + sentinel.
    table_bytes = oft_data[len(_OFT_HEADER):]
    # Last entry is sentinel (total file size); preceding entries are page boundaries.
    num_oft_entries = len(table_bytes) // 4
    if num_oft_entries > 0:
        num_oft_entries -= 1  # exclude sentinel

    # Collect page start offsets: page 0 is implicit at byte 0,
    # remaining pages from OFT entries.
    page_offsets = [0]
    for i in range(num_oft_entries):
        off = struct.unpack_from("<I", table_bytes, i * 4)[0]
        page_offsets.append(off)

    # Walk each 32-entry page and sample entries at positions 0 and 16.
    entries = []
    for page_off in page_offsets:
        pos = page_off
        # Sub-entry 0: first word of the page.
        if pos >= len(idx_data):
            break
        try:
            null = idx_data.index(b"\x00", pos)
        except ValueError:
            break
        word = idx_data[pos:null]
        prefix = word[:_CSPT_PREFIX_LEN].ljust(_CSPT_PREFIX_LEN, b"\x00")
        entries.append(prefix + struct.pack("<I", pos))

        # Skip 16 entries to reach sub-entry 1.
        scan_pos = pos
        for _ in range(16):
            try:
                null = idx_data.index(b"\x00", scan_pos)
            except ValueError:
                scan_pos = len(idx_data)
                break
            scan_pos = null + 1 + 8  # skip null + 8-byte suffix
        if scan_pos >= len(idx_data):
            continue
        try:
            null = idx_data.index(b"\x00", scan_pos)
        except ValueError:
            continue
        word = idx_data[scan_pos:null]
        prefix = word[:_CSPT_PREFIX_LEN].ljust(_CSPT_PREFIX_LEN, b"\x00")
        entries.append(prefix + struct.pack("<I", scan_pos))

    # Build header.
    entry_count = len(entries)
    hdr = _CSPT_MAGIC
    hdr += struct.pack("<B", _CSPT_VERSION)
    hdr += struct.pack("<B", _CSPT_PREFIX_LEN)
    hdr += struct.pack("<H", _CSPT_STRIDE)
    hdr += struct.pack("<I", entry_count)

    return hdr + b"".join(entries)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _parse_all_idx(idx_data: bytes) -> list[tuple[str, int, int]]:
    """Parse all entries from .idx data. Returns [(word, offset, size), ...]."""
    entries = []
    pos = 0
    while pos < len(idx_data):
        null = idx_data.index(b"\x00", pos)
        word = idx_data[pos:null].decode("utf-8")
        offset, size = struct.unpack_from(">II", idx_data, null + 1)
        entries.append((word, offset, size))
        pos = null + 1 + 8
    return entries


def _parse_all_syn(syn_data: bytes) -> list[tuple[str, int]]:
    """Parse all entries from .syn data. Returns [(synonym, word_index), ...]."""
    entries = []
    pos = 0
    while pos < len(syn_data):
        null = syn_data.index(b"\x00", pos)
        word = syn_data[pos:null].decode("utf-8")
        (idx,) = struct.unpack_from(">I", syn_data, null + 1)
        entries.append((word, idx))
        pos = null + 1 + 4
    return entries


def _parse_ifo(ifo_path: Path) -> dict[str, str]:
    """Parse .ifo file into a dict of key=value pairs."""
    result = {}
    for line in ifo_path.read_text(encoding="utf-8").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            result[k] = v
    return result


def _write_ifo(path: Path, fields: dict[str, str]) -> None:
    """Write a StarDict .ifo file."""
    lines = ["StarDict's dict ifo file", "version=2.4.2"]
    for k, v in fields.items():
        lines.append(f"{k}={v}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _find_stem(folder: Path) -> str:
    """Return the StarDict stem by locating the .ifo file in the folder."""
    ifo_files = list(folder.glob("*.ifo"))
    if not ifo_files:
        print(f"ERROR: no .ifo file found in {folder}", file=sys.stderr)
        sys.exit(1)
    if len(ifo_files) > 1:
        print(f"ERROR: multiple .ifo files in {folder} -- cannot determine stem", file=sys.stderr)
        sys.exit(1)
    return ifo_files[0].stem


def _decompress(src: Path, dst: Path) -> None:
    """Gzip-decompress src into dst."""
    with gzip.open(src, "rb") as f_in, open(dst, "wb") as f_out:
        shutil.copyfileobj(f_in, f_out)


# ---------------------------------------------------------------------------
# prep subcommand
# ---------------------------------------------------------------------------

def prep(source_folder: Path) -> None:
    if not source_folder.is_dir():
        print(f"ERROR: not a directory: {source_folder}", file=sys.stderr)
        sys.exit(1)

    stem = _find_stem(source_folder)
    out_dir = source_folder

    print(f"Preparing {source_folder}")

    steps_run = 0

    # Step 1: Extract .dict.dz -> .dict
    dz    = out_dir / f"{stem}.dict.dz"
    dict_ = out_dir / f"{stem}.dict"
    if dz.exists() and not dict_.exists():
        print(f"  Extracting {dz.name} -> {dict_.name} ...", end=" ", flush=True)
        _decompress(dz, dict_)
        print(f"{dict_.stat().st_size / 1024 / 1024:.2f} MB")
        steps_run += 1

    # Step 2: Extract .syn.dz -> .syn
    syn_dz = out_dir / f"{stem}.syn.dz"
    syn    = out_dir / f"{stem}.syn"
    if syn_dz.exists() and not syn.exists():
        print(f"  Extracting {syn_dz.name} -> {syn.name} ...", end=" ", flush=True)
        _decompress(syn_dz, syn)
        print(f"{syn.stat().st_size / 1024 / 1024:.2f} MB")
        steps_run += 1

    # Step 3: Generate .idx.oft
    idx     = out_dir / f"{stem}.idx"
    idx_oft = out_dir / f"{stem}.idx.oft"
    idx_oft_regenerated = False
    if idx.exists() and not idx_oft.exists():
        print(f"  Generating {idx_oft.name} ...", end=" ", flush=True)
        idx_oft.write_bytes(_build_oft(idx.read_bytes(), skip_bytes_after_null=8))
        print(f"{idx_oft.stat().st_size} bytes")
        steps_run += 1
        idx_oft_regenerated = True

    # Step 4: Generate .syn.oft (.syn may have just been created in step 2)
    syn_oft = out_dir / f"{stem}.syn.oft"
    if syn.exists() and not syn_oft.exists():
        print(f"  Generating {syn_oft.name} ...", end=" ", flush=True)
        syn_oft.write_bytes(_build_oft(syn.read_bytes(), skip_bytes_after_null=4))
        print(f"{syn_oft.stat().st_size} bytes")
        steps_run += 1

    # Step 5: Generate .idx.oft.cspt (requires .idx and .idx.oft).
    # Regenerate if .cspt is missing OR if .idx.oft was just rebuilt this run
    # (stale .cspt). Mirrors DictPrepareActivity::detectSteps on-device.
    idx_cspt = out_dir / f"{stem}.idx.oft.cspt"
    if idx.exists() and idx_oft.exists() and (not idx_cspt.exists() or idx_oft_regenerated):
        if idx_cspt.exists():
            idx_cspt.unlink()
        print(f"  Generating {idx_cspt.name} ...", end=" ", flush=True)
        idx_cspt.write_bytes(_build_cspt(idx.read_bytes(), idx_oft.read_bytes()))
        print(f"{idx_cspt.stat().st_size} bytes")
        steps_run += 1

    if steps_run == 0:
        print("  No preparation steps needed -- dictionary already processed.")
    else:
        print(f"  Done. {steps_run} step(s) completed.")
    print(f"  Output: {out_dir}")


# ---------------------------------------------------------------------------
# lookup subcommand
# ---------------------------------------------------------------------------

def _scan_idx(idx_data: bytes, idx_oft_path: Path, word: str) -> tuple[int, int] | None:
    """
    Search idx_data for an exact match of word.
    Returns (dict_offset, size) on match, None if not found.
    Uses .idx.oft.cspt if present, then .idx.oft as fallback.
    """
    target = word.encode("utf-8")
    start_pos = 0
    end_pos = len(idx_data)

    # Try .cspt first; on bad magic / truncation, fall through to .oft.
    cspt_used = False
    cspt_path = Path(str(idx_oft_path) + ".cspt")
    if cspt_path.exists():
        cspt_data = cspt_path.read_bytes()
        if len(cspt_data) >= _CSPT_HEADER_SIZE and cspt_data[:4] == _CSPT_MAGIC:
            prefix_len = cspt_data[4 + 1]  # byte 5
            entry_count = struct.unpack_from("<I", cspt_data, 8)[0]
            entry_size = prefix_len + 4

            if entry_count > 0:
                lo, hi = 0, entry_count - 1
                while lo < hi:
                    mid = lo + (hi - lo + 1) // 2
                    off = _CSPT_HEADER_SIZE + mid * entry_size
                    prefix = cspt_data[off:off + prefix_len].rstrip(b"\x00")
                    if prefix.lower() > target.lower():
                        hi = mid - 1
                    else:
                        lo = mid

                off = _CSPT_HEADER_SIZE + lo * entry_size
                start_pos = struct.unpack_from("<I", cspt_data, off + prefix_len)[0]
                if lo + 1 < entry_count:
                    next_off = _CSPT_HEADER_SIZE + (lo + 1) * entry_size
                    end_pos = struct.unpack_from("<I", cspt_data, next_off + prefix_len)[0]
                else:
                    end_pos = len(idx_data)
            cspt_used = True

    if not cspt_used and idx_oft_path.exists():
        oft_data = idx_oft_path.read_bytes()
        table_bytes = oft_data[len(_OFT_HEADER):]
        count = len(table_bytes) // 4
        best = 0
        for i in range(count):
            pos = struct.unpack_from("<I", table_bytes, i * 4)[0]
            if pos >= len(idx_data):
                break
            try:
                null = idx_data.index(b"\x00", pos)
            except ValueError:
                break
            if idx_data[pos:null].lower() <= target.lower():
                best = pos
            else:
                break
        start_pos = best

    pos = start_pos
    while pos < end_pos:
        try:
            null = idx_data.index(b"\x00", pos)
        except ValueError:
            break
        entry_word = idx_data[pos:null]
        entry_offset, entry_size = struct.unpack_from(">II", idx_data, null + 1)
        pos = null + 1 + 8

        if entry_word == target:
            return entry_offset, entry_size

    return None


def lookup(folder: Path, word: str) -> None:
    if not folder.is_dir():
        print(f"ERROR: not a directory: {folder}", file=sys.stderr)
        sys.exit(1)

    stem = _find_stem(folder)
    dict_path     = folder / f"{stem}.dict"
    dict_dz_path  = folder / f"{stem}.dict.dz"
    idx_path      = folder / f"{stem}.idx"
    idx_oft_path  = folder / f"{stem}.idx.oft"

    if not dict_path.exists():
        if dict_dz_path.exists():
            print(
                f"ERROR: dictionary is still compressed. "
                f"Run: python3 scripts/dict_tools.py prep {folder}",
                file=sys.stderr,
            )
        else:
            print(f"ERROR: {dict_path.name} not found in {folder}", file=sys.stderr)
        sys.exit(1)

    if not idx_path.exists():
        print(f"ERROR: {idx_path.name} not found in {folder}", file=sys.stderr)
        sys.exit(1)

    result = _scan_idx(idx_path.read_bytes(), idx_oft_path, word)

    if result is None:
        print(f"Not found: {word}", file=sys.stderr)
        sys.exit(1)

    entry_offset, entry_size = result
    with open(dict_path, "rb") as f:
        f.seek(entry_offset)
        print(f.read(entry_size).decode("utf-8"))


# ---------------------------------------------------------------------------
# merge subcommand
# ---------------------------------------------------------------------------

def merge(sources: list[Path], output: Path) -> None:
    """Merge multiple StarDict dictionaries into a single dictionary."""
    if len(sources) < 2:
        print("ERROR: merge requires at least 2 source folders", file=sys.stderr)
        sys.exit(1)

    # Dict-based merge: accumulate definitions per headword (no sort needed)
    # word -> list of (src_idx, word_idx) for definition retrieval + synonym remapping
    word_entries: dict[str, list[tuple[int, int]]] = {}
    all_syns: list[tuple[str, int, int]] = []  # (synonym, orig_word_idx, src_idx)
    source_dicts: list[bytes] = []
    source_offsets: list[list[tuple[int, int]]] = []
    sametypesequence: str | None = None

    for src_idx, src_folder in enumerate(sources):
        if not src_folder.is_dir():
            print(f"ERROR: not a directory: {src_folder}", file=sys.stderr)
            sys.exit(1)

        stem = _find_stem(src_folder)
        idx_path = src_folder / f"{stem}.idx"
        dict_path = src_folder / f"{stem}.dict"
        dict_dz = src_folder / f"{stem}.dict.dz"
        syn_path = src_folder / f"{stem}.syn"
        ifo_path = src_folder / f"{stem}.ifo"

        if not dict_path.exists():
            if dict_dz.exists():
                print(
                    f"ERROR: {dict_path.name} not found but .dict.dz exists in {src_folder}. "
                    f"Run: python3 scripts/dictionary_tools.py prep {src_folder}",
                    file=sys.stderr,
                )
            else:
                print(f"ERROR: {dict_path.name} not found in {src_folder}", file=sys.stderr)
            sys.exit(1)

        if not idx_path.exists():
            print(f"ERROR: {idx_path.name} not found in {src_folder}", file=sys.stderr)
            sys.exit(1)

        if ifo_path.exists():
            ifo = _parse_ifo(ifo_path)
            sts = ifo.get("sametypesequence", "")
            if sametypesequence is None:
                sametypesequence = sts
            elif sts and sts != sametypesequence:
                print(
                    f"WARNING: sametypesequence mismatch: "
                    f"'{sametypesequence}' vs '{sts}' in {src_folder}",
                    file=sys.stderr,
                )

        idx_data = idx_path.read_bytes()
        dict_data = dict_path.read_bytes()
        entries = _parse_all_idx(idx_data)

        source_dicts.append(dict_data)
        source_offsets.append([(offset, size) for _, offset, size in entries])

        for word_idx, (word, _, _) in enumerate(entries):
            word_entries.setdefault(word, []).append((src_idx, word_idx))

        if syn_path.exists():
            for syn_word, target_idx in _parse_all_syn(syn_path.read_bytes()):
                all_syns.append((syn_word, target_idx, src_idx))

    # Sort headwords once (just strings, no payloads)
    sorted_words = sorted(word_entries.keys(), key=lambda w: (w.lower(), w))

    # Build merged output
    merged_defs: list[bytes] = []
    index_remap: dict[tuple[int, int], int] = {}

    for new_idx, word in enumerate(sorted_words):
        parts: list[bytes] = []
        for src_idx, orig_idx in word_entries[word]:
            index_remap[(src_idx, orig_idx)] = new_idx
            offset, size = source_offsets[src_idx][orig_idx]
            parts.append(source_dicts[src_idx][offset:offset + size])
        merged_defs.append(b"".join(parts))

    # Write output
    output.mkdir(parents=True, exist_ok=True)
    out_stem = output.name

    # .dict
    dict_bytes = bytearray()
    dict_offsets: list[tuple[int, int]] = []
    for defn in merged_defs:
        dict_offsets.append((len(dict_bytes), len(defn)))
        dict_bytes += defn
    (output / f"{out_stem}.dict").write_bytes(dict_bytes)

    # .idx
    idx_parts: list[bytes] = []
    for word, (offset, size) in zip(sorted_words, dict_offsets):
        idx_parts.append(word.encode("utf-8") + b"\x00" + struct.pack(">II", offset, size))
    idx_bytes = b"".join(idx_parts)
    (output / f"{out_stem}.idx").write_bytes(idx_bytes)

    # .syn (with remapped indices, sorted)
    syn_count = 0
    if all_syns:
        remapped: list[tuple[str, int]] = []
        for syn_word, orig_target, src_idx in all_syns:
            new_target = index_remap.get((src_idx, orig_target))
            if new_target is not None:
                remapped.append((syn_word, new_target))
        remapped.sort(key=lambda e: (e[0].lower(), e[0]))
        if remapped:
            syn_bytes = b""
            for syn_word, target in remapped:
                syn_bytes += syn_word.encode("utf-8") + b"\x00" + struct.pack(">I", target)
            (output / f"{out_stem}.syn").write_bytes(syn_bytes)
            syn_count = len(remapped)

    # .ifo
    ifo_fields = {
        "bookname": out_stem,
        "wordcount": str(len(sorted_words)),
        "idxfilesize": str(len(idx_bytes)),
    }
    if sametypesequence:
        ifo_fields["sametypesequence"] = sametypesequence
    if syn_count:
        ifo_fields["synwordcount"] = str(syn_count)
    _write_ifo(output / f"{out_stem}.ifo", ifo_fields)

    # .oft files + .idx.oft.cspt
    idx_oft_bytes = _build_oft(idx_bytes, skip_bytes_after_null=8)
    (output / f"{out_stem}.idx.oft").write_bytes(idx_oft_bytes)
    (output / f"{out_stem}.idx.oft.cspt").write_bytes(
        _build_cspt(idx_bytes, idx_oft_bytes)
    )
    if syn_count:
        syn_data = (output / f"{out_stem}.syn").read_bytes()
        (output / f"{out_stem}.syn.oft").write_bytes(
            _build_oft(syn_data, skip_bytes_after_null=4)
        )

    print(f"Merged {len(sources)} dictionaries -> {output}")
    print(f"  Words: {len(sorted_words)}, Synonyms: {syn_count}")
    print(f"  Dict: {len(dict_bytes)} bytes, Idx: {len(idx_bytes)} bytes")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Offline StarDict dictionary tools for CrossPoint Reader.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_prep = sub.add_parser("prep", help="Pre-process a dictionary folder")
    p_prep.add_argument("folder", help="Path to the dictionary folder to process")

    p_lookup = sub.add_parser("lookup", help="Look up a word in a prepared dictionary")
    p_lookup.add_argument("folder", help="Path to the prepared dictionary folder")
    p_lookup.add_argument("word", help="Word to look up (exact match, case-sensitive)")

    p_merge = sub.add_parser("merge", help="Merge multiple StarDict dictionaries into one")
    p_merge.add_argument(
        "--source", action="append", required=True, dest="sources",
        help="Path to a source dictionary folder (specify multiple times)",
    )
    p_merge.add_argument(
        "--output", required=True,
        help="Path to the output dictionary folder",
    )

    args = parser.parse_args()

    if args.command == "prep":
        prep(Path(args.folder))
    elif args.command == "lookup":
        lookup(Path(args.folder), args.word)
    elif args.command == "merge":
        merge([Path(s) for s in args.sources], Path(args.output))


if __name__ == "__main__":
    main()
