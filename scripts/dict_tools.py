#!/usr/bin/env python3
"""
dict_tools.py — Offline StarDict dictionary tools for CrossPoint Reader.

Subcommands:
  prep   — Pre-process a dictionary (decompress, generate offset files)
  lookup — Look up a word in a prepared dictionary

Run 'python3 scripts/dict_tools.py <subcommand> --help' for details.
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
# Helpers
# ---------------------------------------------------------------------------

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

    project_root = Path(__file__).parent.parent
    out_dir = project_root / "dictionaries" / stem

    # Copy source files into output directory.
    if out_dir.exists():
        shutil.rmtree(out_dir)
    shutil.copytree(source_folder, out_dir)
    print(f"Copied {source_folder} -> {out_dir}")

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
    if idx.exists() and not idx_oft.exists():
        print(f"  Generating {idx_oft.name} ...", end=" ", flush=True)
        idx_oft.write_bytes(_build_oft(idx.read_bytes(), skip_bytes_after_null=8))
        print(f"{idx_oft.stat().st_size} bytes")
        steps_run += 1

    # Step 4: Generate .syn.oft (.syn may have just been created in step 2)
    syn_oft = out_dir / f"{stem}.syn.oft"
    if syn.exists() and not syn_oft.exists():
        print(f"  Generating {syn_oft.name} ...", end=" ", flush=True)
        syn_oft.write_bytes(_build_oft(syn.read_bytes(), skip_bytes_after_null=4))
        print(f"{syn_oft.stat().st_size} bytes")
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
    Uses idx_oft_path as a jump table (if present) to skip ahead before scanning.
    """
    target = word.encode("utf-8")
    start_pos = 0

    if idx_oft_path.exists():
        oft_data = idx_oft_path.read_bytes()
        # Header is 38 bytes; remaining bytes are uint32 LE file positions,
        # one per _STRIDE entries (position of the start of the next stride block).
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
            # Case-fold comparison to find a safe lower bound in the sorted index.
            if idx_data[pos:null].lower() <= target.lower():
                best = pos
            else:
                break
        start_pos = best

    pos = start_pos
    while pos < len(idx_data):
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

    args = parser.parse_args()

    if args.command == "prep":
        prep(Path(args.folder))
    elif args.command == "lookup":
        lookup(Path(args.folder), args.word)


if __name__ == "__main__":
    main()
