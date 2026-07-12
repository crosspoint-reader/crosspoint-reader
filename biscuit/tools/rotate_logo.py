#!/usr/bin/env python3
"""
rotate_logo.py — Rotate the 120x120 1-bit bitmap in Logo120.h by 90 degrees.

Default: 90° counter-clockwise  (compensates for CW display rotation)
  --cw : 90° clockwise           (try this if CCW doesn't look right)

The bitmap is stored MSB-first: byte 0 of each row holds pixels 0-7,
where bit 7 of that byte is pixel column 0.

Usage:
  python tools/rotate_logo.py        # CCW (default)
  python tools/rotate_logo.py --cw   # CW
"""

import argparse
import re
import sys

HEADER_PATH = "src/images/Logo120.h"
N = 120           # image is NxN pixels
BYTES_PER_ROW = N // 8   # 15


# ---------------------------------------------------------------------------
# Pixel helpers (1-bit, MSB-first packing)
# ---------------------------------------------------------------------------

def get_pixel(data: list[int], x: int, y: int) -> int:
    """Return the bit value (0 or 1) at column x, row y."""
    byte_index = y * BYTES_PER_ROW + (x // 8)
    bit_index  = 7 - (x % 8)   # MSB is the leftmost pixel in each byte
    return (data[byte_index] >> bit_index) & 1


def set_pixel(data: list[int], x: int, y: int, value: int) -> None:
    """Set the bit at column x, row y to value (0 or 1)."""
    byte_index = y * BYTES_PER_ROW + (x // 8)
    bit_index  = 7 - (x % 8)
    if value:
        data[byte_index] |= (1 << bit_index)
    else:
        data[byte_index] &= ~(1 << bit_index)


# ---------------------------------------------------------------------------
# Rotation
# ---------------------------------------------------------------------------

def rotate_ccw(src: list[int]) -> list[int]:
    """
    90° counter-clockwise rotation.
    new_pixel(x, y) = old_pixel(N-1-y, x)
    """
    dst = [0] * (N * BYTES_PER_ROW)
    for y in range(N):
        for x in range(N):
            set_pixel(dst, x, y, get_pixel(src, N - 1 - y, x))
    return dst


def rotate_cw(src: list[int]) -> list[int]:
    """
    90° clockwise rotation.
    new_pixel(x, y) = old_pixel(y, N-1-x)
    """
    dst = [0] * (N * BYTES_PER_ROW)
    for y in range(N):
        for x in range(N):
            set_pixel(dst, x, y, get_pixel(src, y, N - 1 - x))
    return dst


# ---------------------------------------------------------------------------
# File I/O
# ---------------------------------------------------------------------------

def parse_header(path: str) -> list[int]:
    """Read Logo120.h and return all hex byte values as a flat list of ints."""
    with open(path, "r") as f:
        content = f.read()

    # Find everything inside the array braces
    match = re.search(r"Logo120\[\]\s*=\s*\{([^}]*)\}", content, re.DOTALL)
    if not match:
        sys.exit(f"ERROR: could not find Logo120[] array in {path}")

    array_body = match.group(1)
    values = [int(tok, 16) for tok in re.findall(r"0x[0-9A-Fa-f]{2}", array_body)]

    expected = N * BYTES_PER_ROW
    if len(values) != expected:
        sys.exit(
            f"ERROR: expected {expected} bytes ({N} rows x {BYTES_PER_ROW} bytes), "
            f"got {len(values)}"
        )

    return values


def format_header(data: list[int]) -> str:
    """Render the rotated data as a C header string."""
    lines = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("// Biscuit logo 120x120, 1-bit")
    lines.append("// Auto-generated - do not edit manually")
    lines.append("")
    lines.append("static const uint8_t Logo120[] = {")

    for row in range(N):
        offset = row * BYTES_PER_ROW
        row_bytes = data[offset : offset + BYTES_PER_ROW]
        hex_values = ", ".join(f"0x{b:02X}" for b in row_bytes)
        lines.append(f"  {hex_values},")

    lines.append("};")
    lines.append("")   # trailing newline

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Rotate the Logo120.h 1-bit bitmap 90 degrees."
    )
    parser.add_argument(
        "--cw",
        action="store_true",
        help="Rotate 90° clockwise instead of the default counter-clockwise.",
    )
    args = parser.parse_args()

    direction = "clockwise" if args.cw else "counter-clockwise"
    print(f"Rotating Logo120.h 90° {direction}...")

    data = parse_header(HEADER_PATH)
    print(f"  Parsed {len(data)} bytes ({N}x{N} pixels, {BYTES_PER_ROW} bytes/row)")

    rotated = rotate_cw(data) if args.cw else rotate_ccw(data)

    output = format_header(rotated)
    with open(HEADER_PATH, "w") as f:
        f.write(output)

    print(f"  Written to {HEADER_PATH}")
    print("Done. Rebuild the firmware to see the result.")
    print()
    print("If the logo still looks wrong, run the script again with the opposite flag:")
    if args.cw:
        print("  python tools/rotate_logo.py         # try CCW instead")
    else:
        print("  python tools/rotate_logo.py --cw    # try CW instead")


if __name__ == "__main__":
    main()
