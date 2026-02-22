#!/usr/bin/env python3
"""
Generate test BMP images for verifying Bitmap.cpp format support.

Creates BMP files at 480x800 (CrossPoint display in portrait orientation).
Test images use patterns designed to reveal dithering artifacts:
  - Checkerboard: sharp edges between gray levels, dithering adds noise at boundaries
  - Fine lines: thin 1px lines on contrasting background, dithering smears them
  - Mixed blocks: small rectangles of alternating grays, dithering blurs transitions
  - Gradient band: smooth transition in the middle, clean grays top/bottom

Formats generated:
- 1-bit: black & white (baseline, never dithered)
- 2-bit: 4-level grayscale (non-standard CrossPoint extension, won't open on PC)
- 4-bit: 4-color grayscale palette (standard BMP, new support)
- 8-bit: 4-color grayscale palette (colorsUsed=4, should skip dithering)
- 8-bit: 256-color grayscale (full palette, should be dithered)
- 24-bit: RGB grayscale gradient (should be dithered)

Usage:
    python generate_test_bmps.py [output_dir]
    Default output_dir: ./test_bmps/
"""

import struct
import os
import sys

WIDTH = 480
HEIGHT = 800

# The 4 e-ink gray levels (luminance values)
GRAY_LEVELS = [0, 85, 170, 255]


def write_bmp_file_header(f, pixel_data_offset, file_size):
    f.write(b'BM')
    f.write(struct.pack('<I', file_size))
    f.write(struct.pack('<HH', 0, 0))  # reserved
    f.write(struct.pack('<I', pixel_data_offset))


def write_bmp_dib_header(f, width, height, bpp, colors_used=0):
    f.write(struct.pack('<I', 40))  # DIB header size (BITMAPINFOHEADER)
    f.write(struct.pack('<i', width))
    f.write(struct.pack('<i', -height))  # negative = top-down
    f.write(struct.pack('<HH', 1, bpp))  # planes, bpp
    f.write(struct.pack('<I', 0))  # compression (BI_RGB)
    f.write(struct.pack('<I', 0))  # image size (can be 0 for BI_RGB)
    f.write(struct.pack('<i', 0))  # X pixels per meter
    f.write(struct.pack('<i', 0))  # Y pixels per meter
    f.write(struct.pack('<I', colors_used))
    f.write(struct.pack('<I', 0))  # important colors


def write_palette(f, entries):
    """Write palette entries as BGRA (B, G, R, 0x00)."""
    for gray in entries:
        f.write(struct.pack('BBBB', gray, gray, gray, 0))


def get_test_pattern_index(x, y, width, height):
    """Return palette index (0-3) for a test pattern designed to reveal dithering.

    Layout (4 horizontal bands):
      Band 1 (top 25%):    Checkerboard of all 4 gray levels in 16x16 blocks
      Band 2 (25-50%):     Fine horizontal lines (1px) alternating gray levels
      Band 3 (50-75%):     4 vertical stripes, each with a nested smaller pattern
      Band 4 (bottom 25%): Diagonal gradient-like pattern using the 4 levels
    """
    band = y * 4 // height

    if band == 0:
        # Checkerboard: 16x16 blocks cycling through all 4 gray levels
        bx = (x // 16) % 4
        by = (y // 16) % 4
        return (bx + by) % 4

    elif band == 1:
        # Fine lines: 1px horizontal lines alternating between two gray levels
        # Left half: alternates black/white, Right half: alternates dark/light gray
        if x < width // 2:
            return 0 if (y % 2 == 0) else 3  # black/white
        else:
            return 1 if (y % 2 == 0) else 2  # dark gray/light gray

    elif band == 2:
        # 4 vertical stripes, each containing a smaller checkerboard at 4x4
        stripe = min(x * 4 // width, 3)
        inner = ((x // 4) + (y // 4)) % 2
        if stripe == 0:
            return 0 if inner else 1  # black / dark gray
        elif stripe == 1:
            return 1 if inner else 2  # dark gray / light gray
        elif stripe == 2:
            return 2 if inner else 3  # light gray / white
        else:
            return 0 if inner else 3  # black / white (max contrast)

    else:
        # Diagonal bands using all 4 levels
        return ((x + y) // 20) % 4


def get_test_pattern_lum(x, y, width, height):
    """Return 0-255 luminance for a continuous-tone test pattern.

    Same layout as index version but uses full grayscale range
    to show how dithering handles non-native gray values.
    """
    band = y * 4 // height

    if band == 0:
        # Checkerboard with intermediate grays (not native levels)
        bx = (x // 16) % 4
        by = (y // 16) % 4
        # Use values between native levels to force dithering decisions
        values = [0, 64, 128, 192]
        return values[(bx + by) % 4]

    elif band == 1:
        # Fine lines with intermediate values
        if x < width // 2:
            return 32 if (y % 2 == 0) else 224  # near-black / near-white
        else:
            return 96 if (y % 2 == 0) else 160  # mid-grays

    elif band == 2:
        # Smooth horizontal gradient across full width
        return (x * 255) // (width - 1)

    else:
        # Diagonal bands with smooth transitions
        return ((x + y) * 255 // (width + height))


def generate_1bit(path):
    """1-bit BMP: checkerboard pattern."""
    bpp = 1
    palette = [0, 255]
    row_bytes = (WIDTH * bpp + 31) // 32 * 4
    palette_size = len(palette) * 4
    pixel_offset = 14 + 40 + palette_size
    file_size = pixel_offset + row_bytes * HEIGHT

    with open(path, 'wb') as f:
        write_bmp_file_header(f, pixel_offset, file_size)
        write_bmp_dib_header(f, WIDTH, HEIGHT, bpp, len(palette))
        write_palette(f, palette)

        for y in range(HEIGHT):
            row = bytearray(row_bytes)
            for x in range(WIDTH):
                # 16x16 checkerboard
                val = ((x // 16) + (y // 16)) % 2
                if val:
                    row[x >> 3] |= (0x80 >> (x & 7))
            f.write(row)

    print(f"  Created: {path} ({bpp}-bit, {len(palette)} colors)")


def generate_2bit(path):
    """2-bit BMP: 4-level grayscale test pattern (non-standard, CrossPoint extension)."""
    bpp = 2
    palette = GRAY_LEVELS
    row_bytes = (WIDTH * bpp + 31) // 32 * 4
    palette_size = len(palette) * 4
    pixel_offset = 14 + 40 + palette_size
    file_size = pixel_offset + row_bytes * HEIGHT

    with open(path, 'wb') as f:
        write_bmp_file_header(f, pixel_offset, file_size)
        write_bmp_dib_header(f, WIDTH, HEIGHT, bpp, len(palette))
        write_palette(f, palette)

        for y in range(HEIGHT):
            row = bytearray(row_bytes)
            for x in range(WIDTH):
                idx = get_test_pattern_index(x, y, WIDTH, HEIGHT)
                byte_pos = x >> 2
                bit_shift = 6 - ((x & 3) * 2)
                row[byte_pos] |= (idx << bit_shift)
            f.write(row)

    print(f"  Created: {path} ({bpp}-bit, {len(palette)} colors)")


def generate_4bit(path):
    """4-bit BMP: 4-color grayscale test pattern (standard, should skip dithering)."""
    bpp = 4
    palette = GRAY_LEVELS
    row_bytes = (WIDTH * bpp + 31) // 32 * 4
    palette_size = len(palette) * 4
    pixel_offset = 14 + 40 + palette_size
    file_size = pixel_offset + row_bytes * HEIGHT

    with open(path, 'wb') as f:
        write_bmp_file_header(f, pixel_offset, file_size)
        write_bmp_dib_header(f, WIDTH, HEIGHT, bpp, len(palette))
        write_palette(f, palette)

        for y in range(HEIGHT):
            row = bytearray(row_bytes)
            for x in range(WIDTH):
                idx = get_test_pattern_index(x, y, WIDTH, HEIGHT)
                byte_pos = x >> 1
                if x & 1:
                    row[byte_pos] |= idx
                else:
                    row[byte_pos] |= (idx << 4)
            f.write(row)

    print(f"  Created: {path} ({bpp}-bit, {len(palette)} colors)")


def generate_8bit_4colors(path):
    """8-bit BMP with only 4 palette entries (colorsUsed=4, should skip dithering)."""
    bpp = 8
    palette = GRAY_LEVELS
    row_bytes = (WIDTH * bpp + 31) // 32 * 4
    palette_size = len(palette) * 4
    pixel_offset = 14 + 40 + palette_size
    file_size = pixel_offset + row_bytes * HEIGHT

    with open(path, 'wb') as f:
        write_bmp_file_header(f, pixel_offset, file_size)
        write_bmp_dib_header(f, WIDTH, HEIGHT, bpp, len(palette))
        write_palette(f, palette)

        for y in range(HEIGHT):
            row = bytearray(row_bytes)
            for x in range(WIDTH):
                row[x] = get_test_pattern_index(x, y, WIDTH, HEIGHT)
            f.write(row)

    print(f"  Created: {path} ({bpp}-bit, {len(palette)} colors)")


def generate_8bit_256colors(path):
    """8-bit BMP with full 256 palette (should be dithered normally)."""
    bpp = 8
    palette = list(range(256))
    row_bytes = (WIDTH * bpp + 31) // 32 * 4
    palette_size = len(palette) * 4
    pixel_offset = 14 + 40 + palette_size
    file_size = pixel_offset + row_bytes * HEIGHT

    with open(path, 'wb') as f:
        write_bmp_file_header(f, pixel_offset, file_size)
        write_bmp_dib_header(f, WIDTH, HEIGHT, bpp, len(palette))
        write_palette(f, palette)

        for y in range(HEIGHT):
            row = bytearray(row_bytes)
            for x in range(WIDTH):
                row[x] = get_test_pattern_lum(x, y, WIDTH, HEIGHT)
            f.write(row)

    print(f"  Created: {path} ({bpp}-bit, {len(palette)} colors)")


def generate_24bit(path):
    """24-bit BMP: RGB grayscale test pattern (should be dithered normally)."""
    bpp = 24
    row_bytes = (WIDTH * bpp + 31) // 32 * 4
    pixel_offset = 14 + 40
    file_size = pixel_offset + row_bytes * HEIGHT

    with open(path, 'wb') as f:
        write_bmp_file_header(f, pixel_offset, file_size)
        write_bmp_dib_header(f, WIDTH, HEIGHT, bpp, 0)

        for y in range(HEIGHT):
            row = bytearray(row_bytes)
            for x in range(WIDTH):
                gray = get_test_pattern_lum(x, y, WIDTH, HEIGHT)
                offset = x * 3
                row[offset] = gray      # B
                row[offset + 1] = gray  # G
                row[offset + 2] = gray  # R
            f.write(row)

    print(f"  Created: {path} ({bpp}-bit)")


def main():
    output_dir = sys.argv[1] if len(sys.argv) > 1 else './test_bmps'
    os.makedirs(output_dir, exist_ok=True)

    print(f"Generating test BMPs in {output_dir}/")
    print(f"Resolution: {WIDTH}x{HEIGHT}")
    print()

    generate_1bit(os.path.join(output_dir, 'test_1bit_bw.bmp'))
    generate_2bit(os.path.join(output_dir, 'test_2bit_4gray.bmp'))
    generate_4bit(os.path.join(output_dir, 'test_4bit_4gray.bmp'))
    generate_8bit_4colors(os.path.join(output_dir, 'test_8bit_4colors.bmp'))
    generate_8bit_256colors(os.path.join(output_dir, 'test_8bit_256gray_gradient.bmp'))
    generate_24bit(os.path.join(output_dir, 'test_24bit_gradient.bmp'))

    print()
    print("Test pattern layout (4 horizontal bands):")
    print("  Band 1 (top):    16x16 checkerboard cycling all 4 gray levels")
    print("  Band 2:          Fine 1px horizontal lines alternating grays")
    print("  Band 3:          4 stripes with nested 4x4 checkerboard detail")
    print("  Band 4 (bottom): Diagonal bands cycling all 4 levels")
    print()
    print("What to look for:")
    print("  Direct mapping:  Sharp, clean edges between gray blocks")
    print("  Dithering:       Noisy/speckled boundaries, smeared fine lines")
    print()
    print("Expected results on device:")
    print("  1-bit:  Clean B&W checkerboard, no dithering")
    print("  2-bit:  Clean 4-gray pattern, no dithering  (non-standard BMP, won't open on PC)")
    print("  4-bit:  Clean 4-gray pattern, no dithering  (standard BMP, viewable on PC)")
    print("  8-bit (4 colors):   Clean 4-gray pattern, no dithering  (standard BMP, viewable on PC)")
    print("  8-bit (256 colors): Same layout but with intermediate grays, WITH dithering")
    print("  24-bit: Same layout but with intermediate grays, WITH dithering")
    print()
    print("Note: 2-bit BMP is a non-standard CrossPoint extension. Standard image viewers")
    print("will not open it. Use the 4-bit BMP instead for images created with standard tools")
    print("(e.g. ImageMagick: convert input.png -colorspace Gray -colors 4 -depth 4 BMP3:output.bmp)")
    print()
    print("Copy files to /sleep/ folder on SD card to test as custom sleep screen images.")


if __name__ == '__main__':
    main()
