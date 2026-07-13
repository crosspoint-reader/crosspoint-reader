#!/usr/bin/env python3
"""Convert a StarDict or dictd dictionary into the CrossPoint .cpd format.

The .cpd format is designed for low-RAM lookup on the device: a sorted
fixed-width index is binary-searched directly on the SD card (36 bytes read
per probe), so no part of the dictionary is ever loaded into RAM.

Layout (all integers little-endian):
    0   char[4]  magic "CPD1"
    4   uint32   entryCount
    8   uint32   indexOffset   (absolute file offset of the first index record)
    12  uint32   entriesOffset (absolute file offset of the definitions blob)
    16  uint8    keyLen        (fixed index key size, KEY_LEN below)
    17  uint8    flags         (bit0: keys were normalized with Turkish casing)
    18  uint16   reserved
    20  char[12] language pair, NUL-padded (e.g. "en-tr")
    32  char[64] dictionary title, UTF-8, NUL-padded
    96  index records: entryCount * { char key[keyLen]; uint32 off; uint32 len }
        sorted by memcmp() order of key; off is relative to entriesOffset
    ... definitions blob: concatenated UTF-8 plain text (no NUL terminators)

Keys are lowercased with the same Latin/Latin-Ext-A mapping the firmware's
HyphenationCommon uses (plus dotted/dotless I when --turkish-keys is given),
so device-side normalization produces identical bytes. Distinct headwords
whose truncated keys collide are merged into one entry.

Usage:
    python scripts/make_dictionary.py INPUT -o eng-tur.cpd \
        [--lang en-tr] [--title "..."] [--turkish-keys] [--max-entry-bytes N]

INPUT may be a StarDict .ifo file (with .idx[.gz] and .dict[.dz] siblings)
or a dictd .index file (with a .dict[.dz] sibling).

Requirements: none beyond the Python standard library.
"""

from __future__ import annotations

import argparse
import gzip
import html
import pathlib
import re
import struct
import sys
import unicodedata

KEY_LEN = 28
HEADER_LEN = 96
MAGIC = b"CPD1"
FLAG_TURKISH_KEYS = 0x01


# ---------------------------------------------------------------------------
# Key normalization — must mirror toLowerLatin()/toLowerTurkish() in
# lib/Epub/Epub/hyphenation/HyphenationCommon.cpp so that device-side
# normalization produces byte-identical keys.
# ---------------------------------------------------------------------------

def _lower_latin_cp(cp: int) -> int:
    if ord('A') <= cp <= ord('Z'):
        return cp - ord('A') + ord('a')
    if 0x00C0 <= cp <= 0x00D6 or 0x00D8 <= cp <= 0x00DE:
        return cp + 0x20
    if (0x0100 <= cp <= 0x0137 and cp % 2 == 0) or \
       (0x0139 <= cp <= 0x0148 and cp % 2 == 1) or \
       (0x014A <= cp <= 0x0177 and cp % 2 == 0) or \
       (0x0179 <= cp <= 0x017E and cp % 2 == 1):
        return cp + 1
    if cp == 0x0178:  # Ÿ
        return 0x00FF
    if cp == 0x1E9E:  # ẞ
        return 0x00DF
    return cp


def normalize_key(word: str, turkish: bool) -> bytes:
    word = unicodedata.normalize("NFC", word.strip())
    out = []
    for ch in word:
        cp = ord(ch)
        if turkish:
            if cp == ord('I'):
                out.append('ı')  # ı
                continue
            if cp == 0x0130:  # İ
                out.append('i')
                continue
        out.append(chr(_lower_latin_cp(cp)))
    return "".join(out).encode("utf-8")[:KEY_LEN]


# ---------------------------------------------------------------------------
# Definition cleanup
# ---------------------------------------------------------------------------

_BLOCK_TAGS = re.compile(r"<\s*(?:br|/p|/div|/li|/tr)\b[^>]*>", re.I)
_TAGS = re.compile(r"<[^>]+>")


def strip_markup(text: str) -> str:
    """Reduce StarDict 'h'/'x' entry markup to plain text with line breaks."""
    text = _BLOCK_TAGS.sub("\n", text)
    text = _TAGS.sub("", text)
    text = html.unescape(text)
    lines = [re.sub(r"[ \t]+", " ", ln).strip() for ln in text.split("\n")]
    out, blank = [], False
    for ln in lines:
        if ln:
            out.append(ln)
            blank = False
        elif not blank and out:
            out.append("")
            blank = True
    return "\n".join(out).strip()


def clean_entry(raw: bytes, typechar: str) -> str:
    text = raw.decode("utf-8", errors="replace")
    if typechar in ("h", "x", "g"):
        return strip_markup(text)
    return text.strip()


# ---------------------------------------------------------------------------
# Input parsers
# ---------------------------------------------------------------------------

def _open_maybe_gz(path: pathlib.Path) -> bytes:
    if path.suffix in (".gz", ".dz"):
        return gzip.open(path, "rb").read()
    return path.read_bytes()


def _find_sibling(base: pathlib.Path, stem: str, exts: list[str]) -> pathlib.Path:
    for ext in exts:
        p = base / (stem + ext)
        if p.exists():
            return p
    raise FileNotFoundError(f"none of {stem}{exts} next to {base}")


def iterate_stardict(ifo_path: pathlib.Path):
    info = {}
    for line in ifo_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            info[k.strip()] = v.strip()
    if info.get("idxoffsetbits", "32") != "32":
        sys.exit("64-bit idxoffsetbits is not supported")
    typeseq = info.get("sametypesequence", "")
    if typeseq and len(typeseq) != 1:
        sys.exit(f"multi-part sametypesequence '{typeseq}' is not supported")
    title = info.get("bookname", ifo_path.stem)

    stem = ifo_path.name[:-4]
    idx = _open_maybe_gz(_find_sibling(ifo_path.parent, stem, [".idx", ".idx.gz"]))
    dic = _open_maybe_gz(_find_sibling(ifo_path.parent, stem, [".dict", ".dict.dz"]))

    def gen():
        pos = 0
        while pos < len(idx):
            end = idx.index(b"\0", pos)
            word = idx[pos:end].decode("utf-8", errors="replace")
            off, size = struct.unpack(">II", idx[end + 1:end + 9])
            pos = end + 9
            raw = dic[off:off + size]
            if typeseq:
                yield word, raw, typeseq
            else:
                # Untyped entries carry a one-char type prefix per chunk; only
                # the single-chunk case is supported.
                yield word, raw[1:], chr(raw[0]) if raw else "m"

    return title, gen()


_B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"


def _dictd_num(s: str) -> int:
    n = 0
    for c in s:
        n = n * 64 + _B64.index(c)
    return n


def iterate_dictd(index_path: pathlib.Path):
    stem = index_path.name[:-len(".index")]
    dic = _open_maybe_gz(_find_sibling(index_path.parent, stem, [".dict", ".dict.dz"]))

    def gen():
        for line in index_path.read_text(encoding="utf-8", errors="replace").splitlines():
            parts = line.split("\t")
            if len(parts) < 3 or parts[0].startswith("00-database"):
                continue
            word, off, size = parts[0], _dictd_num(parts[1]), _dictd_num(parts[2])
            yield word, dic[off:off + size], "m"

    return stem, gen()


# ---------------------------------------------------------------------------
# Writer
# ---------------------------------------------------------------------------

def build(args) -> None:
    src = pathlib.Path(args.input)
    if src.suffix == ".ifo":
        title, it = iterate_stardict(src)
    elif src.name.endswith(".index"):
        title, it = iterate_dictd(src)
    else:
        sys.exit("input must be a StarDict .ifo or a dictd .index file")
    if args.title:
        title = args.title

    merged: dict[bytes, list[str]] = {}
    skipped = 0
    for word, raw, typechar in it:
        key = normalize_key(word, args.turkish_keys)
        if not key:
            skipped += 1
            continue
        text = clean_entry(raw, typechar)
        if not text:
            skipped += 1
            continue
        merged.setdefault(key, []).append(text)

    keys = sorted(merged)
    blob = bytearray()
    records = []
    truncated_entries = 0
    for key in keys:
        text = "\n\n".join(merged[key])
        data = text.encode("utf-8")
        if len(data) > args.max_entry_bytes:
            data = data[:args.max_entry_bytes]
            # do not split a UTF-8 sequence
            while data and (data[-1] & 0xC0) == 0x80:
                data = data[:-1]
            data += b"\xe2\x80\xa6"  # ellipsis
            truncated_entries += 1
        records.append((key, len(blob), len(data)))
        blob += data

    index_offset = HEADER_LEN
    entries_offset = index_offset + len(records) * (KEY_LEN + 8)

    flags = FLAG_TURKISH_KEYS if args.turkish_keys else 0
    header = struct.pack(
        "<4sIIIBBH12s64s",
        MAGIC, len(records), index_offset, entries_offset,
        KEY_LEN, flags, 0,
        args.lang.encode("utf-8")[:12],
        title.encode("utf-8")[:64],
    )
    assert len(header) == HEADER_LEN

    out = pathlib.Path(args.output)
    with out.open("wb") as f:
        f.write(header)
        for key, off, length in records:
            f.write(struct.pack(f"<{KEY_LEN}sII", key, off, length))
        f.write(blob)

    print(f"{out}: {len(records)} entries, index {entries_offset - index_offset} B, "
          f"definitions {len(blob)} B, total {out.stat().st_size} B")
    if skipped:
        print(f"  skipped {skipped} empty/unusable source entries")
    if truncated_entries:
        print(f"  truncated {truncated_entries} entries to {args.max_entry_bytes} B")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("input", help="StarDict .ifo or dictd .index file")
    p.add_argument("-o", "--output", required=True, help="output .cpd path")
    p.add_argument("--lang", default="", help="language pair label, e.g. en-tr")
    p.add_argument("--title", default="", help="override dictionary title")
    p.add_argument("--turkish-keys", action="store_true",
                   help="normalize headword case with Turkish dotted/dotless I")
    p.add_argument("--max-entry-bytes", type=int, default=8192,
                   help="clamp a single definition to this many bytes (default 8192)")
    build(p.parse_args())


if __name__ == "__main__":
    main()
