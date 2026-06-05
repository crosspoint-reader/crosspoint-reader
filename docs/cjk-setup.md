# Installing CJK Fonts on CrossPoint 1.3.0

This document explains how to generate and install Chinese (CJK) fonts for
CrossPoint 1.3.0's on-demand SD-card font system.

## Prerequisites

- A TTF or OTF file containing CJK glyphs (e.g., `NotoSansCJKsc-Regular.otf`).
- Python 3 with PyYAML.
- The `fontconvert_sdcard.py` script at `lib/EpdFont/scripts/fontconvert_sdcard.py`.

## Generation

```bash
python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
  --intervals cjk \
  --sizes 14,16,18 --style regular \
  NotoSansCJKsc-Regular.otf \
  --output-dir NotoSansCJK/
```

This produces three 2-bit anti-aliased `.cpfont` files in `NotoSansCJK/`:
- `NotoSansCJK-14.cpfont`
- `NotoSansCJK-16.cpfont`
- `NotoSansCJK-18.cpfont`

## Installation

> **Note:** On Windows and macOS, leading-dot directories (e.g., `/.fonts/`) are hidden by default. Enable 'Show hidden files' in your file manager or use a CLI tool to confirm the directory exists.

1. Copy the `.cpfont` files to the SD card at `/.fonts/NotoSansCJK/`. The
   **directory name** is the family name that `FontSelectionActivity`
   (see `src/activities/settings/FontSelectionActivity.cpp:35-44`) reads
   back from the on-device font registry. The family name must match
   the directory name exactly.
2. Eject the SD card safely and reinsert into the Xteink X4.
3. Open the font selector on-device (the activity that sets
   `SETTINGS.sdFontFamilyName`, defined in `src/CrossPointSettings.h:219`).
   The directory `/.fonts/NotoSansCJK/` will appear as a family named
   `NotoSansCJK`. Select it. `SETTINGS.sdFontFamilyName` is set to the
   directory name verbatim, e.g., `"NotoSansCJK"`.
4. Open a CJK EPUB or TXT file. The reader will prewarm CJK glyphs on
   first page turn; subsequent page turns use the in-RAM interval table
   and overflow ring buffer.

### How the family name matches the directory

The font registry (`FontSelectionActivity.cpp:35`) compares each
`SdFontFamily::name` against `SETTINGS.sdFontFamilyName` byte-for-byte.
The family name is taken from the directory name under `/.fonts/`. So:

- `/.fonts/NotoSansCJK/` → family name `"NotoSansCJK"`
- `/.fonts/Noto Sans CJK/` → family name `"Noto Sans CJK"` (note the
  space; the SD card stores the directory verbatim, but some
  filesystems disallow spaces — prefer underscores or CamelCase)
- `/.fonts/notosanscjk/` → family name `"notosanscjk"` (case-sensitive
  on case-sensitive filesystems; recommend matching the casing used at
  generation time)

If the family does not appear in the selector, the directory is missing,
unreadable, or contains no `.cpfont` files.

## Verifying it works

- The serial monitor should show prewarm log lines on the first page of a CJK book.
- The first-line paragraph indent (2x 我 width) should be visible.
- The 2-bit anti-aliasing should give smooth diagonal strokes on CJK glyphs.

## Troubleshooting

- **"font not found" in the language selector**: verify the file is at
  `/.fonts/NotoSansCJK/`, not at `/fonts/...`.
- **Pages render as missing-glyph boxes**: the `.cpfont` was generated
  without `--intervals cjk` and lacks the CJK codepoint range.
  Regenerate with the correct flag.
- **Out of heap during prewarm**: reduce `MAX_PAGE_GLYPHS` from 512 to
  256 in `lib/EpdFont/SdCardFont.h:23` and rebuild.
