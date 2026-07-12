# XTC/XTCH Library

XTC ebook format support for CrossPoint Reader.

## Supported Formats

| Format | Extension | Description                                  |
|--------|-----------|----------------------------------------------|
| XTC    | `.xtc`    | Container with XTG pages (1-bit monochrome)  |
| XTCH   | `.xtch`   | Container with XTH pages (2-bit grayscale)   |

## Format Overview

XTC/XTCH are container formats designed for ESP32 e-paper displays. They store pre-rendered bitmap pages optimized for the XTeink X4 e-reader (480x800 resolution).

### Container Structure (XTC/XTCH)

- 56-byte header with metadata offsets
- Optional metadata (title, author, etc.)
- Page index table (16 bytes per page)
- Page data (XTG or XTH format)

### Page Formats

#### XTG (1-bit monochrome)

- Row-major storage, 8 pixels per byte
- MSB first (bit 7 = leftmost pixel)
- 0 = Black, 1 = White

#### XTH (2-bit grayscale)

- Two bit planes stored sequentially
- Column-major order (right to left)
- 8 vertical pixels per byte
- Grayscale: 0=White, 1=Dark Grey, 2=Light Grey, 3=Black

## Reference

Original format info: <https://gist.github.com/CrazyCoder/b125f26d6987c0620058249f59f1327d>
