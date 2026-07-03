#!/usr/bin/env python3
"""
dict_tools.py — Offline StarDict dictionary tools for CrossPoint Reader.

Subcommands:
  prep   — Pre-process a dictionary (validate dictzip, extract index/synonym data, generate offset files)
  lookup — Look up a word in a prepared dictionary
  merge  — Merge multiple StarDict dictionaries into one

Run 'python3 scripts/dictionary_tools.py <subcommand> --help' for details.
"""

import argparse
import bisect
import gzip
import shutil
import struct
import sys
from pathlib import Path




# ---------------------------------------------------------------------------
# .fpi constants — the Fenced Prefix Index sidecar (tuning: fencep_6 + tab1_3).
# Minimizes SD sector reads for exact-match lookup by narrowing the search range
# against a small sidecar copy of sector-boundary words instead of the source file;
# see the "Fenced Prefix Index" section of docs/dictionary-development.md for the
# motivation and the benchmark that picked this tuning. Supersedes .cspt for
# exact-match lookup (Dictionary::locate / resolveAltForm).
# No self-description beyond a version byte; prefix lengths are fixed constants,
# mirrored exactly in src/util/Dictionary.cpp and test/data/generate_dictionaries.py.
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


def _build_fpi(src_data: bytes, skip_per_entry: int) -> bytes:
    """Build .fpi bytes for .idx (skip=8) or .syn (skip=4).

    Single combined sidecar: sector 0 = a version byte + a "tab" table of up to
    170 3-byte lowercase prefixes, evenly sampled across source-file 512-byte
    sectors; sectors 1..N = a "fencep" sidecar of front-coded 6-byte prefixes, one
    per source-file sector, grouped 8 sectors at a time, plus a per-group 4-byte LE
    cumulative-ordinal field (the local==0 sector's fence word's 0-based entry
    ordinal, or the total entry count as a sentinel past the last entry) consumed
    by Dictionary::binarySearchFpiOrdinal. Mirrors Dictionary::generateFpi
    (src/util/Dictionary.cpp) byte-for-byte — the host has no RAM constraint so
    this parses the whole file up front rather than streaming.
    """
    src_len = len(src_data)
    sector_count = 0 if src_len == 0 else (src_len + _FPI_SECTOR_SIZE - 1) // _FPI_SECTOR_SIZE
    tab_entry_count = min(_FPI_TAB_ENTRY_CAP, sector_count)

    entries = []  # [(lowercased_word, entry_start), ...] in file order
    pos = 0
    while pos < src_len:
        null = src_data.index(b"\x00", pos)
        entries.append((src_data[pos:null].lower(), pos))
        pos = null + 1 + skip_per_entry
    starts = [e[1] for e in entries]

    # Each source-file sector's fence word: the first entry whose start is >=
    # sector*512 (a single long entry can be the fence word for multiple
    # consecutive sectors); trailing sectors past the last entry stay empty.
    # fence_ordinal defaults to entries' total count — a valid sentinel upper
    # bound for ordinal search when a sector is past the last entry.
    fence_word = [b""] * sector_count
    fence_start = [0] * sector_count
    fence_ordinal = [len(entries)] * sector_count
    for sector in range(sector_count):
        i = bisect.bisect_left(starts, sector * _FPI_SECTOR_SIZE)
        if i < len(entries):
            fence_word[sector], fence_start[sector] = entries[i]
            fence_ordinal[sector] = i

    # Tab table.
    tab = bytearray(_FPI_SECTOR_SIZE - _FPI_HEADER_SIZE)
    for i in range(tab_entry_count):
        sector = _fpi_sampled_sector(i, tab_entry_count, sector_count)
        w = fence_word[sector][:_FPI_TAB_PREFIX_LEN]
        off = i * _FPI_TAB_PREFIX_LEN
        tab[off:off + len(w)] = w

    # Fencep sidecar: front-coded within each 8-sector group, sidecar sectors
    # padded to exactly 512 bytes (including full ones — a group is 61 bytes, so
    # 8 groups only fill 488 of a sidecar sector's 512 bytes).
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


def _validate_dictzip(path: Path) -> tuple[int, int]:
    """Validate a dictzip RA table. Returns (chunk_len, chunk_count)."""
    data = path.read_bytes()
    if len(data) < 18 or data[:3] != b"\x1f\x8b\x08":
        raise ValueError("not a gzip file")
    flg = data[3]
    if (flg & 0x04) == 0:
        raise ValueError("missing FEXTRA")

    pos = 10
    xlen = struct.unpack_from("<H", data, pos)[0]
    pos += 2
    extra = data[pos:pos + xlen]
    if len(extra) != xlen:
        raise ValueError("truncated extra field")

    i = 0
    while i + 4 <= len(extra):
        si1 = extra[i]
        si2 = extra[i + 1]
        sub_len = struct.unpack_from("<H", extra, i + 2)[0]
        i += 4
        sub = extra[i:i + sub_len]
        if len(sub) != sub_len:
            raise ValueError("truncated subfield")
        if si1 == ord("R") and si2 == ord("A"):
            if sub_len < 6:
                raise ValueError("short RA subfield")
            version, chunk_len, chunk_count = struct.unpack_from("<HHH", sub, 0)
            if version != 1:
                raise ValueError(f"unsupported RA version {version}")
            if chunk_len == 0 or chunk_count == 0:
                raise ValueError("empty RA table")
            if sub_len != 6 + chunk_count * 2:
                raise ValueError("RA table length mismatch")
            return chunk_len, chunk_count
        i += sub_len

    raise ValueError("missing RA subfield")


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

    # Step 1: Validate .dict.dz
    dz    = out_dir / f"{stem}.dict.dz"
    dict_ = out_dir / f"{stem}.dict"
    if dz.exists() and not dict_.exists():
        try:
            chunk_len, chunk_count = _validate_dictzip(dz)
        except ValueError as exc:
            print(f"ERROR: invalid dictzip file {dz}: {exc}", file=sys.stderr)
            sys.exit(1)
        print(f"  Validated {dz.name} ({chunk_count} chunks @ {chunk_len} bytes)")
        steps_run += 1

    # Step 2: Extract .idx.gz -> .idx
    idx_gz = out_dir / f"{stem}.idx.gz"
    idx = out_dir / f"{stem}.idx"
    if idx_gz.exists() and not idx.exists():
        print(f"  Extracting {idx_gz.name} -> {idx.name} ...", end=" ", flush=True)
        _decompress(idx_gz, idx)
        print(f"{idx.stat().st_size / 1024 / 1024:.2f} MB")
        steps_run += 1

    # Step 3: Extract .syn.dz -> .syn
    syn_dz = out_dir / f"{stem}.syn.dz"
    syn    = out_dir / f"{stem}.syn"
    if syn_dz.exists() and not syn.exists():
        print(f"  Extracting {syn_dz.name} -> {syn.name} ...", end=" ", flush=True)
        _decompress(syn_dz, syn)
        print(f"{syn.stat().st_size / 1024 / 1024:.2f} MB")
        steps_run += 1

    # Step 4: Generate .idx.fpi (Fenced Prefix Index — the exact-match
    # lookup fast path, plus a per-group ordinal field consumed by
    # Dictionary::wordAtOrdinal/findSimilar).
    idx_fpi = out_dir / f"{stem}.idx.fpi"
    if idx.exists() and not idx_fpi.exists():
        print(f"  Generating {idx_fpi.name} ...", end=" ", flush=True)
        idx_fpi.write_bytes(_build_fpi(idx.read_bytes(), skip_per_entry=8))
        print(f"{idx_fpi.stat().st_size} bytes")
        steps_run += 1

    # Step 5: Generate .syn.fpi.
    syn_fpi = out_dir / f"{stem}.syn.fpi"
    if syn.exists() and not syn_fpi.exists():
        print(f"  Generating {syn_fpi.name} ...", end=" ", flush=True)
        syn_fpi.write_bytes(_build_fpi(syn.read_bytes(), skip_per_entry=4))
        print(f"{syn_fpi.stat().st_size} bytes")
        steps_run += 1

    if steps_run == 0:
        print("  No preparation steps needed -- dictionary already processed.")
    else:
        print(f"  Done. {steps_run} step(s) completed.")
    print(f"  Output: {out_dir}")


# ---------------------------------------------------------------------------
# lookup subcommand
# ---------------------------------------------------------------------------

def _fpi_ascii_cmp_ci(a: bytes, b: bytes) -> int:
    n = min(len(a), len(b))
    al, bl = a[:n].lower(), b[:n].lower()
    if al != bl:
        return -1 if al < bl else 1
    if len(a) < len(b):
        return -1
    if len(a) > len(b):
        return 1
    return 0


def _fpi_tab_prefix_cmp(tab_entry: bytes, target: bytes) -> int:
    """Port of dict_index_benchmark.go's tabPrefixCmp / Dictionary::fpiTabPrefixCmp:
    a shorter target that's a case-insensitive prefix of the (longer) stored tab
    entry compares as equal — ambiguous, so the search must not rule it out."""
    if len(target) < len(tab_entry) and _fpi_ascii_cmp_ci(tab_entry[:len(target)], target) == 0:
        return 0
    return _fpi_ascii_cmp_ci(tab_entry, target)


def _fpi_narrow_from_sidecar_sector(raw: bytes, sector_count: int, base_sector: int, target: bytes, start: int,
                                    end: int) -> tuple[int, int]:
    """Port of Dictionary::fpiNarrowFromSidecarSector / narrowFencePCBounds: decode one
    already-read fencep sidecar sector and narrow [start, end)."""
    prev = b""
    have_last_le = False
    last_le_start = 0

    for group_in_sector in range(_FPI_GROUPS_PER_SIDECAR_SECTOR):
        group = raw[group_in_sector * _FPI_GROUP_SIZE:(group_in_sector + 1) * _FPI_GROUP_SIZE]
        for local in range(_FPI_GROUP_SECTORS):
            sector = base_sector + group_in_sector * _FPI_GROUP_SECTORS + local
            if sector >= sector_count:
                if have_last_le and last_le_start > start:
                    start = last_le_start
                return start, end

            if local == 0:
                prev = b""

            rel = group[local]
            if group[8] & (1 << local):
                rel |= 0x100
            entry_start = sector * _FPI_SECTOR_SIZE + rel

            slot_off = 9 + local * _FPI_FENCEP_PREFIX_LEN
            slot = group[slot_off:slot_off + _FPI_FENCEP_PREFIX_LEN]
            if local != 0 and (slot[0] & 0x80):
                common = min(slot[0] & 0x7f, len(prev))
                cur = prev[:common] + slot[1:]
            else:
                cur = slot
            prev = cur

            null_idx = cur.find(b"\x00")
            confirmed = null_idx >= 0
            real = cur[:null_idx] if confirmed else cur

            is_gt = False
            if confirmed:
                if _fpi_ascii_cmp_ci(real, target) <= 0:
                    last_le_start, have_last_le = entry_start, True
                else:
                    is_gt = True
            elif len(target) >= len(real) and _fpi_ascii_cmp_ci(target[:len(real)], real) == 0:
                pass  # target starts with this incomplete prefix — ambiguous, skip.
            elif _fpi_ascii_cmp_ci(real, target) < 0:
                last_le_start, have_last_le = entry_start, True
            else:
                is_gt = True

            if is_gt:
                if have_last_le and last_le_start > start:
                    start = last_le_start
                if entry_start < end:
                    end = entry_start
                return start, end

    if have_last_le and last_le_start > start:
        start = last_le_start
    return start, end


def _search_fpi(idx_data: bytes, fpi_data: bytes, word: str) -> tuple[int, int] | None:
    """Binary search a .fpi sidecar (Fenced Prefix Index) for the byte range in
    idx_data containing word. Returns (start, end), or None if .fpi is missing/
    invalid/unusable (caller should fall back to a full linear scan)."""
    src_len = len(idx_data)
    sector_count = 0 if src_len == 0 else (src_len + _FPI_SECTOR_SIZE - 1) // _FPI_SECTOR_SIZE
    if sector_count == 0 or len(fpi_data) < _FPI_SECTOR_SIZE or fpi_data[0] != _FPI_VERSION:
        return None
    tab_entry_count = min(_FPI_TAB_ENTRY_CAP, sector_count)
    target = word.encode("utf-8")

    tab = fpi_data[_FPI_HEADER_SIZE:_FPI_SECTOR_SIZE]

    def tab_entry(i: int) -> bytes:
        raw = tab[i * _FPI_TAB_PREFIX_LEN:(i + 1) * _FPI_TAB_PREFIX_LEN]
        return raw.split(b"\x00", 1)[0]

    # Tab comparisons always use target truncated to _FPI_TAB_PREFIX_LEN (mirrors the
    # Go reference's `targetPrefix := prefix(target, len(entries[0].prefix))`, computed
    # once and reused for the initial search AND both group-widening loops below).
    # Without this truncation, a target longer than a tab entry never compares as
    # _fpi_tab_prefix_cmp's ambiguous "==0" case (it falls through to the length
    # tiebreak instead), which silently defeats the widening loops when most entries
    # share the same prefix.
    tab_target = target[:_FPI_TAB_PREFIX_LEN]

    lo, hi = 0, tab_entry_count - 1
    while lo < hi:
        mid = lo + (hi - lo + 1) // 2
        if _fpi_tab_prefix_cmp(tab_entry(mid), tab_target) > 0:
            hi = mid - 1
        else:
            lo = mid

    group_lo = lo
    while group_lo > 0 and _fpi_tab_prefix_cmp(tab_entry(group_lo - 1), tab_target) == 0:
        group_lo -= 1
    group_hi = lo
    while group_hi + 1 < tab_entry_count and _fpi_tab_prefix_cmp(tab_entry(group_hi + 1), tab_target) == 0:
        group_hi += 1

    lo_sector = _fpi_sampled_sector(group_lo - 1, tab_entry_count, sector_count) if group_lo > 0 else 0
    hi_sector = (
        _fpi_sampled_sector(group_hi + 1, tab_entry_count, sector_count)
        if group_hi + 1 < tab_entry_count
        else sector_count - 1
    )
    if hi_sector < lo_sector:
        hi_sector = lo_sector

    start = lo_sector * _FPI_SECTOR_SIZE
    end = (hi_sector + 1) * _FPI_SECTOR_SIZE if hi_sector + 1 < sector_count else src_len
    end = min(end, src_len)

    fencep = fpi_data[_FPI_SECTOR_SIZE:]
    sectors_per_sidecar = _FPI_GROUPS_PER_SIDECAR_SECTOR * _FPI_GROUP_SECTORS

    while end > start:
        lo_s, hi_s = start // _FPI_SECTOR_SIZE, (end - 1) // _FPI_SECTOR_SIZE
        if lo_s >= hi_s:
            break
        old_start, old_end = start, end
        mid = lo_s + (hi_s - lo_s + 1) // 2
        sidecar_sector = mid // sectors_per_sidecar
        sec_off = sidecar_sector * _FPI_SECTOR_SIZE
        raw = fencep[sec_off:sec_off + _FPI_SECTOR_SIZE]
        if len(raw) < _FPI_SECTOR_SIZE:
            break

        start, end = _fpi_narrow_from_sidecar_sector(raw, sector_count, sidecar_sector * sectors_per_sidecar, target,
                                                       start, end)

        if start == old_start and end == old_end:
            break
        if (start // _FPI_SECTOR_SIZE) // sectors_per_sidecar == sidecar_sector and (
            (end - 1) // _FPI_SECTOR_SIZE
        ) // sectors_per_sidecar == sidecar_sector:
            break

    if end <= start:
        return None
    return start, end


def _scan_idx(idx_data: bytes, idx_fpi_path: Path, word: str) -> tuple[int, int] | None:
    """
    Search idx_data for an exact match of word.
    Returns (dict_offset, size) on match, None if not found.
    Uses .idx.fpi (Fenced Prefix Index) to narrow the scan range if present and valid;
    falls back to a full linear scan otherwise. Mirrors Dictionary::locate.
    """
    target = word.encode("utf-8")
    start_pos = 0
    end_pos = len(idx_data)

    if idx_fpi_path.exists():
        bounds = _search_fpi(idx_data, idx_fpi_path.read_bytes(), word)
        if bounds is not None:
            start_pos, end_pos = bounds

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
    idx_fpi_path  = folder / f"{stem}.idx.fpi"

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

    result = _scan_idx(idx_path.read_bytes(), idx_fpi_path, word)

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
                    f"ERROR: sametypesequence mismatch: "
                    f"'{sametypesequence}' vs '{sts}' in {src_folder}. "
                    f"All sources must share the same sametypesequence to merge.",
                    file=sys.stderr,
                )
                sys.exit(1)

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
        merged_defs.append(b"\n".join(parts))

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

    # .fpi files (Fenced Prefix Index — the exact-match lookup fast path, plus a
    # per-group ordinal field for wordAtOrdinal/findSimilar).
    (output / f"{out_stem}.idx.fpi").write_bytes(_build_fpi(idx_bytes, skip_per_entry=8))
    if syn_count:
        syn_data = (output / f"{out_stem}.syn").read_bytes()
        (output / f"{out_stem}.syn.fpi").write_bytes(_build_fpi(syn_data, skip_per_entry=4))

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
