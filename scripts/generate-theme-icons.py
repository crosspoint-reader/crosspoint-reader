#!/usr/bin/env python3
"""Export compiled 1-bit UI icon headers as BMP assets for SD themes."""

import argparse
import re
import struct
from pathlib import Path


ICON_HEADERS = [
    "book.h",
    "book24.h",
    "bookmark.h",
    "cover.h",
    "file24.h",
    "folder.h",
    "folder24.h",
    "hotspot.h",
    "image24.h",
    "library.h",
    "recent.h",
    "settings2.h",
    "text24.h",
    "transfer.h",
    "wifi.h",
]


def parse_icon_header(path: Path):
    text = path.read_text()
    size_match = re.search(r"//\s*size:\s*(\d+)x(\d+)", text)
    if not size_match:
        raise ValueError(f"missing size comment in {path}")
    width = int(size_match.group(1))
    height = int(size_match.group(2))

    values = [int(m.group(1), 16) for m in re.finditer(r"0x([0-9A-Fa-f]{2})", text)]
    expected = ((width + 7) // 8) * height
    if len(values) != expected:
        raise ValueError(f"{path}: expected {expected} bytes, found {len(values)}")
    return width, height, bytes(values)


def get_bit(bitmap: bytes, width: int, x: int, y: int) -> int:
    stride = (width + 7) // 8
    return (bitmap[y * stride + x // 8] >> (7 - (x % 8))) & 1


def set_bit(buf: bytearray, width: int, x: int, y: int, value: int):
    stride = (width + 7) // 8
    if value:
        buf[y * stride + x // 8] |= 1 << (7 - (x % 8))


def rotate_1bit_cw(width: int, height: int, bitmap: bytes):
    rotated_width = height
    rotated_height = width
    rotated = bytearray(((rotated_width + 7) // 8) * rotated_height)
    for y in range(height):
        for x in range(width):
            set_bit(rotated, rotated_width, height - 1 - y, x, get_bit(bitmap, width, x, y))
    return rotated_width, rotated_height, bytes(rotated)


def write_1bit_bmp(path: Path, width: int, height: int, bitmap: bytes):
    src_stride = (width + 7) // 8
    dst_stride = ((width + 31) // 32) * 4
    pixel_bytes = dst_stride * height
    pixel_offset = 14 + 40 + 8
    file_size = pixel_offset + pixel_bytes

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as out:
      # BITMAPFILEHEADER
      out.write(b"BM")
      out.write(struct.pack("<IHHI", file_size, 0, 0, pixel_offset))
      # BITMAPINFOHEADER. Negative height stores rows top-down.
      out.write(struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 1, 0, pixel_bytes, 0, 0, 2, 0))
      # Palette index 0 = black, index 1 = white. Existing icon arrays use 1s
      # for white/transparent background and 0s for ink.
      out.write(bytes([0, 0, 0, 0, 255, 255, 255, 0]))
      for y in range(height):
          row = bitmap[y * src_stride : (y + 1) * src_stride]
          out.write(row)
          out.write(b"\x00" * (dst_stride - src_stride))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--icons", default="src/components/icons")
    parser.add_argument("--themes", default="sd-themes")
    args = parser.parse_args()

    icon_root = Path(args.icons)
    theme_root = Path(args.themes)

    parsed = []
    for header in ICON_HEADERS:
        icon_path = icon_root / header
        width, height, data = parse_icon_header(icon_path)
        width, height, data = rotate_1bit_cw(width, height, data)
        parsed.append((icon_path.stem, width, height, data))

    for theme_dir in sorted(theme_root.iterdir()):
        if not theme_dir.is_dir() or not (theme_dir / "theme.json").exists():
            continue
        for name, width, height, data in parsed:
            write_1bit_bmp(theme_dir / "icons" / f"{name}.bmp", width, height, data)


if __name__ == "__main__":
    main()
