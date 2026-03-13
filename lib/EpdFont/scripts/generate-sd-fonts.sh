#!/bin/bash
# Generate recommended SD card font packs for CrossPoint.
#
# Prerequisites:
#   pip install freetype-py fonttools
#
# Source fonts are in ../builtinFonts/source/:
#   Bookerly/, NotoSans/, OpenDyslexic/, Ubuntu/ — committed to git
#   NotoSansCJK/ — downloaded automatically by this script (gitignored)
#
# Output goes to ./output/ (copy to SD card at /.crosspoint/fonts/)

set -e
cd "$(dirname "$0")"

SCRIPT="./fontconvert_sdcard.py"
FONT_DIR="../builtinFonts/source"
OUTPUT_BASE="./output"

SIZES="12,14,16,18"

# --- Download fonts that aren't checked into git ---

NOTOSANSCJK_DIR="$FONT_DIR/NotoSansCJK"
NOTOSANSCJK_FONT="$NOTOSANSCJK_DIR/NotoSansCJKsc-Regular.otf"

if [ ! -f "$NOTOSANSCJK_FONT" ]; then
  echo "Downloading NotoSansCJKsc-Regular.otf..."
  mkdir -p "$NOTOSANSCJK_DIR"
  curl -fSL -o "$NOTOSANSCJK_FONT" \
    "https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf"
  echo "Downloaded $(du -h "$NOTOSANSCJK_FONT" | cut -f1) to $NOTOSANSCJK_FONT"
fi

# Clean output directories to ensure a fresh build
echo "Cleaning output directories..."
rm -rf "$OUTPUT_BASE/NotoSansExtended/" "$OUTPUT_BASE/Bookerly-SD/" "$OUTPUT_BASE/NotoSansCJK/"

# Run all three font families in parallel
echo "=== Starting parallel font generation ==="

echo "[1/3] NotoSansExtended (Latin-ext + Greek + Cyrillic + Georgian + Armenian + Ethiopic)"
python3 "$SCRIPT" \
  "$FONT_DIR/NotoSans/NotoSans-Regular.ttf" \
  --intervals latin-ext,greek,cyrillic,georgian,armenian,ethiopic \
  --sizes "$SIZES" --style regular \
  --name NotoSansExtended \
  --output-dir "$OUTPUT_BASE/NotoSansExtended/" &
PID_NOTO=$!

echo "[2/3] Bookerly-SD (multi-style)"
python3 "$SCRIPT" \
  --regular "$FONT_DIR/Bookerly/Bookerly-Regular.ttf" \
  --bold "$FONT_DIR/Bookerly/Bookerly-Bold.ttf" \
  --italic "$FONT_DIR/Bookerly/Bookerly-Italic.ttf" \
  --bolditalic "$FONT_DIR/Bookerly/Bookerly-BoldItalic.ttf" \
  --intervals builtin \
  --sizes "$SIZES" --force-autohint \
  --name Bookerly-SD \
  --output-dir "$OUTPUT_BASE/Bookerly-SD/" &
PID_BOOKERLY=$!

echo "[3/3] NotoSansCJK (CJK + ASCII + Punctuation)"
python3 "$SCRIPT" \
  "$FONT_DIR/NotoSansCJK/NotoSansCJKsc-Regular.otf" \
  --intervals ascii,latin1,punctuation,cjk \
  --sizes "$SIZES" --style regular \
  --name NotoSansCJK \
  --output-dir "$OUTPUT_BASE/NotoSansCJK/" &
PID_CJK=$!

# Wait for all and track failures
FAILED=0
wait $PID_NOTO || { echo "ERROR: NotoSansExtended generation failed"; FAILED=1; }
wait $PID_BOOKERLY || { echo "ERROR: Bookerly-SD generation failed"; FAILED=1; }
wait $PID_CJK || { echo "ERROR: NotoSansCJK generation failed"; FAILED=1; }

if [ $FAILED -ne 0 ]; then
  echo "=== Some font generations failed ==="
  exit 1
fi

echo ""
echo "=== Done ==="
echo "Copy the contents of $OUTPUT_BASE/ to your SD card at /.crosspoint/fonts/"
