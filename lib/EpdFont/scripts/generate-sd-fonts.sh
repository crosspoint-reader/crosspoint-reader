#!/bin/bash
# Generate recommended SD card font packs for CrossPoint.
#
# Prerequisites:
#   pip install freetype-py
#
# Source fonts should be in ../builtinFonts/source/:
#   NotoSans/NotoSans-Regular.ttf (already in repo)
#   NotoSansCJK/NotoSansCJKsc-Regular.otf (already in repo)
#
# Output goes to ./output/ (copy to SD card at /.crosspoint/fonts/)

set -e
cd "$(dirname "$0")"

SCRIPT="./fontconvert_sdcard.py"
FONT_DIR="../builtinFonts/source"
OUTPUT_BASE="./output"

SIZES="12,14,16,18"

echo "=== Generating NotoSansExtended (Latin-ext + Greek + Cyrillic + Georgian + Armenian + Ethiopic) ==="
python3 "$SCRIPT" \
  "$FONT_DIR/NotoSans/NotoSans-Regular.ttf" \
  --intervals latin-ext,greek,cyrillic,georgian,armenian,ethiopic \
  --sizes "$SIZES" --style regular --2bit \
  --name NotoSansExtended \
  --output-dir "$OUTPUT_BASE/NotoSansExtended/"

echo ""
echo "=== Generating Bookerly-SD (all styles, matching built-in intervals) ==="
BOOKERLY_STYLES=("regular" "bold" "italic" "bolditalic")
BOOKERLY_STYLE_FILES=("Regular" "Bold" "Italic" "BoldItalic")
for i in "${!BOOKERLY_STYLES[@]}"; do
  style="${BOOKERLY_STYLES[$i]}"
  style_file="${BOOKERLY_STYLE_FILES[$i]}"
  echo "--- Style: $style ---"
  python3 "$SCRIPT" \
    "$FONT_DIR/Bookerly/Bookerly-${style_file}.ttf" \
    --intervals builtin \
    --sizes "$SIZES" --style "$style" --2bit --force-autohint \
    --name Bookerly-SD \
    --output-dir "$OUTPUT_BASE/Bookerly-SD/"
  echo ""
done

echo ""
echo "=== Generating NotoSansCJK (CJK + ASCII + Punctuation) ==="
python3 "$SCRIPT" \
  "$FONT_DIR/NotoSansCJK/NotoSansCJKsc-Regular.otf" \
  --intervals ascii,latin1,punctuation,cjk \
  --sizes "$SIZES" --style regular --2bit \
  --name NotoSansCJK \
  --output-dir "$OUTPUT_BASE/NotoSansCJK/"

echo ""
echo "=== Done ==="
echo "Copy the contents of $OUTPUT_BASE/ to your SD card at /.crosspoint/fonts/"
