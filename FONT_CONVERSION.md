# Font Conversion Guide

To use custom fonts with the CrossPoint Reader, you must convert standard `.ttf` or `.otf` font files into the specific `.epdfont` binary format used by the compiled firmware.

We use a Python script located at `lib/EpdFont/scripts/fontconvert.py`.

## Requirements
- Python 3
- `freetype-py` library (`pip install freetype-py`)

## Usage

Run the script from the project root:

```bash
python3 lib/EpdFont/scripts/fontconvert.py --binary [Family-Style-Size] [Size] [PathToFont]
```

### Arguments
1. `name`: The output filename (without extension). **Convention:** `Family-Style-Size` (e.g. `Bookerly-Regular-12`).
2. `size`: The integer point size (e.g. `12`).
3. `fontstack`: Path to the source font file (e.g. `fonts/Bookerly-Regular.ttf`).
4. `--binary`: **REQUIRED**. Flags the script to output the `.epdfont` binary instead of a C header.

### Example

To convert `Bookerly-Regular.ttf` to a size 12 font:

```bash
python3 lib/EpdFont/scripts/fontconvert.py --binary Bookerly-Regular-12 12 fonts/Bookerly-Regular.ttf
```

This will generate `Bookerly-Regular-12.epdfont` in your current directory.

## Installing on Device
1. Rename the file if necessary to match the pattern: `Family-Style-Size.epdfont`.
2. Copy the `.epdfont` file to the `/fonts` directory on your SD card.
