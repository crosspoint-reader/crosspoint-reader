#!/usr/bin/env python3
"""Compress a generated 1-bit UI face header in place: its flat glyph bitmap becomes DEFLATE groups.

A TRANSFORM of the header fontconvert.py already wrote, never a regeneration: it reads the bitmap
bytes the face ships today and re-encodes exactly those. Regenerating from the source fonts would
not reproduce them -- the CJK UI faces were cut on another machine, and the Python converter is not
byte-stable across FreeType builds -- so compressing by transformation is what makes "every glyph
decodes to the bytes it has today" true by construction rather than by luck. The script proves it
before writing anything: every group is inflated back and every glyph compared.

What changes in the header, and nothing else does:
  * <name>Bitmaps holds the concatenated group streams instead of the flat bitmap;
  * <name>BitmapOffsets is gone -- a compressed face recovers each glyph's position by walking its
    group (FontDecompressor::getAlignedOffset), so the table would be dead weight;
  * <name>Groups is new, in the EpdFontGroup layout the reader faces use, ringBytes included;
  * <name>PackedBytes / <name>PackedCrc32 describe the flat bitmap the groups decode back to, so a
    consumer that materialises the face (CjkUiFonts, into the language pack slot) can size and check
    its copy without walking the glyphs;
  * the EpdFontData initialiser points at the groups.
Glyph records, intervals, kerning and every shared table are untouched, so metrics cannot move.

The group payload is the 1-bit bitmap with each ROW padded to a byte, not the continuous bit stream
the flat array holds: identical rows then produce identical bytes wherever they start, which DEFLATE
can match. Measured on the six CJK UI faces at 2 KB groups: 90,968 B against 104,471 B for the packed
stream. FontDecompressor::GroupStream repacks each glyph as it is read, as it does for the 2-bit faces.

Why 2 KB groups. A glyph is reached by decoding its group from the start, so the cap bounds the
worst-case cost of one glyph on the C3 (~0.4 ms per group start plus ~2.5 us per byte, host-scaled
from the X3 reader_cold benchmark): ~5 ms at 2 KB against ~10 ms at 4 KB, for 4.4 KB more flash
across the CJK faces. The ring a group needs can never exceed the group, so 2 KB also caps the
transient RAM of a decode.

Prior art: crosspoint-reader PR #4017 (Justin Mitchell) compresses that project's UI faces too, as the
packed 1-bit stream in 2 KB groups, inflated whole into the decompressor's hot-group buffer and read
in place. None of its code is used here, and the design differs where this tree differs: our
FontDecompressor streams a group through a ring and repacks each glyph rather than keeping a hot
group, so the rows can be byte-aligned for the smaller stream; the faces are compressed by
transforming their headers rather than regenerated; and the active UI language's faces are not
decoded per glyph at all (see build_cjk_ui_fonts.py).

Usage:  python compress_ui_face.py <builtinFonts/face.h> [...] [--group-bytes 2048]
A header that is already compressed is left alone, so re-running over a tree is safe.
"""

import argparse
import re
import sys
import zlib
from pathlib import Path

import deflate_groups

# Must match FontDecompressor::HOT_GLYPH_BUF_SIZE: the per-glyph fallback cache a compressed glyph is
# decoded into. fontconvert.py checks the same bound for the faces it compresses itself.
HOT_GLYPH_BUF_SIZE = 576
# The ring ceiling the reader faces are encoded against (fontconvert.py GROUP_RING_MAX_BYTES). A UI
# group at the default cap can never need more than its own size, so this only binds a larger cap.
GROUP_RING_MAX_BYTES = 4096
DEFAULT_GROUP_BYTES = 2048


def array_re(ctype, symbol, sized):
    size = r"\[\d+\]" if sized else r"\[\]"
    return re.compile(r"static const " + ctype + " " + re.escape(symbol) + size + r" = \{\n(.*?)\n\};\n", re.S)


def row_aligned(packed, width, height):
    """The continuous 1-bit stream of one glyph, with every row padded to a whole byte."""
    if width == 0 or height == 0:
        return b""
    stride = (width + 7) // 8
    out = bytearray(stride * height)
    for y in range(height):
        for x in range(width):
            p = y * width + x
            if (packed[p >> 3] >> (7 - (p & 7))) & 1:
                out[y * stride + (x >> 3)] |= 0x80 >> (x & 7)
    return bytes(out)


def packed_from_rows(rows, width, height):
    """Inverse of row_aligned: what the device's GroupStream produces for one glyph."""
    if width == 0 or height == 0:
        return b""
    stride = (width + 7) // 8
    out = bytearray((width * height + 7) // 8)
    for y in range(height):
        for x in range(width):
            if (rows[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1:
                p = y * width + x
                out[p >> 3] |= 0x80 >> (p & 7)
    return bytes(out)


def chunks(seq, n):
    return [seq[i:i + n] for i in range(0, len(seq), n)]


def compress_header(path: Path, group_bytes: int) -> str:
    text = path.read_text(encoding="utf-8")
    name = path.stem
    if re.search(r"static const EpdFontGroup " + re.escape(name) + r"Groups\[\]", text):
        return "already compressed"
    if " * mode: 1-bit\n" not in text:
        raise SystemExit(f"{path.name}: not an uncompressed 1-bit face (no '* mode: 1-bit' line)")

    bm = array_re("uint8_t", name + "Bitmaps", True).search(text)
    gl = array_re("EpdGlyphPacked", name + "Glyphs", False).search(text)
    of = array_re("uint16_t", name + "BitmapOffsets", False).search(text)
    fd = re.search(r"static const EpdFontData " + re.escape(name) + r" = \{\n(.*?)\n\};", text, re.S)
    if not (bm and gl and of and fd):
        raise SystemExit(f"{path.name}: expected Bitmaps, Glyphs, BitmapOffsets and EpdFontData {name}")

    bitmap = bytes(int(h, 16) for h in re.findall(r"0x([0-9A-Fa-f]{2})", bm.group(1)))
    glyphs = [tuple(int(v) for v in g) for g in
              re.findall(r"\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\}", gl.group(1))]
    offsets = [int(v) for v in re.findall(r"^\s*(\d+),", of.group(1), re.M)]
    if len(offsets) != len(glyphs):
        raise SystemExit(f"{path.name}: {len(glyphs)} glyphs but {len(offsets)} bitmap offsets")

    # The flat layout the device indexes today: each glyph's continuous bit stream, back to back.
    packed = []
    run = 0
    for i, (w, h, _adv, _left, _top) in enumerate(glyphs):
        n = (w * h + 7) // 8
        if offsets[i] != run:
            raise SystemExit(f"{path.name}: glyph {i} offset {offsets[i]} is not the running sum {run}")
        if n > HOT_GLYPH_BUF_SIZE:
            raise SystemExit(f"{path.name}: glyph {i} packs to {n} B, over HOT_GLYPH_BUF_SIZE={HOT_GLYPH_BUF_SIZE}")
        packed.append(bitmap[run:run + n])
        run += n
    if run != len(bitmap):
        raise SystemExit(f"{path.name}: glyphs cover {run} of {len(bitmap)} bitmap bytes")

    rows = [row_aligned(p, g[0], g[1]) for p, g in zip(packed, glyphs)]

    # Contiguous runs of glyphs in array order, closed before the next glyph would pass the cap.
    groups = []  # (first, count)
    first, size = 0, 0
    for i, r in enumerate(rows):
        if i > first and size + len(r) > group_bytes:
            groups.append((first, i - first))
            first, size = i, 0
        size += len(r)
    groups.append((first, len(rows) - first))

    stream = bytearray()
    entries = []
    for first, count in groups:
        blob = b"".join(rows[first:first + count])
        if not blob:
            raise SystemExit(f"{path.name}: group at glyph {first} has no pixels")
        raw, ring = deflate_groups.encode_block(blob, GROUP_RING_MAX_BYTES, use_zopfli=True)
        # Prove it before writing: the stream inflates to the rows, the rows repack to today's bytes.
        rows_back = zlib.decompress(raw, -15)
        assert rows_back == blob, f"{path.name}: group at glyph {first} does not round-trip"
        at = 0
        for i in range(first, first + count):
            w, h = glyphs[i][0], glyphs[i][1]
            n = ((w + 7) // 8) * h
            assert packed_from_rows(rows_back[at:at + n], w, h) == packed[i], f"{path.name}: glyph {i} differs"
            at += n
        entries.append((len(stream), len(raw), len(blob), count, ring, first))
        stream.extend(raw)

    crc = zlib.crc32(bitmap) & 0xFFFFFFFF
    peak_ring = max(e[4] for e in entries)

    out = text
    out = out.replace(" * mode: 1-bit\n",
                      f" * mode: 1-bit  compressed: zopfli, rows byte-aligned, {group_bytes} B groups "
                      f"(compress_ui_face.py)\n", 1)
    body = "\n".join("    " + " ".join(f"0x{b:02X}," for b in c) for c in chunks(stream, 16))
    out = out.replace(bm.group(0), f"static const uint8_t {name}Bitmaps[{len(stream)}] = {{\n{body}\n}};\n", 1)
    out = out.replace(of.group(0) + "\n", "", 1) if (of.group(0) + "\n") in out else out.replace(of.group(0), "", 1)

    group_rows = "\n".join(f"    {{ {a}, {b}, {c}, {d}, {e}, {f} }}," for a, b, c, d, e, f in entries)
    extra = (f"static const EpdFontGroup {name}Groups[] = {{\n{group_rows}\n}};\n\n"
             f"// The flat bitmap the groups decode back to -- the uncompressed face's bitmap, byte for byte.\n"
             f"// What a consumer that materialises the face sizes and checks its copy against.\n"
             f"static constexpr uint32_t {name}PackedBytes = {len(bitmap)};\n"
             f"static constexpr uint32_t {name}PackedCrc32 = 0x{crc:08X};\n\n")
    anchor = f"static const EpdFontData {name} = {{"
    out = out.replace(anchor, extra + anchor, 1)

    # The initialiser, field by field: 4 bitmapOffsets, 11 groups, 12 groupCount (1-based).
    lines = fd.group(1).split("\n")
    fields = [i for i, l in enumerate(lines) if re.sub(r"//.*", "", l).strip()]
    def field(k):
        return re.sub(r"//.*", "", lines[fields[k - 1]]).strip().rstrip(",")
    if field(4) != f"{name}BitmapOffsets" or field(10) != "false" or field(11) != "nullptr" or field(12) != "0":
        raise SystemExit(f"{path.name}: EpdFontData initialiser is not the uncompressed 1-bit layout")
    lines[fields[3]] = "    nullptr,  // bitmapOffsets: a compressed font recovers the offset from the group walk"
    lines[fields[10]] = f"    {name}Groups,"
    lines[fields[11]] = f"    {len(entries)},"
    new_fd = fd.group(0).replace(fd.group(1), "\n".join(lines), 1)
    out = out.replace(fd.group(0), new_fd, 1)

    path.write_text(out, encoding="utf-8", newline="\n")
    return (f"{len(bitmap)} + {2 * len(glyphs)} B offsets -> {len(stream)} + {20 * len(entries)} B groups "
            f"({len(entries)} groups, peak ring {peak_ring} B)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("headers", nargs="+", type=Path)
    ap.add_argument("--group-bytes", type=int, default=DEFAULT_GROUP_BYTES,
                    help="cap on a group's decoded (row-aligned) bytes; default %(default)s")
    args = ap.parse_args()
    for h in args.headers:
        print(f"{h.name}: {compress_header(h, args.group_bytes)}")


if __name__ == "__main__":
    sys.exit(main())
