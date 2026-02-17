#!/bin/bash

set -e

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
BOOKERLY_FONT_SIZES=(12 14 16 18)
NOTOSANS_FONT_SIZES=(12 14 16 18)
OPENDYSLEXIC_FONT_SIZES=(8 10 12 14)

# ── Create temp dir for binary font files ────────────────────────────────────
BINDIR="$(mktemp -d)"
trap "rm -rf $BINDIR" EXIT

echo "=== Generating font binaries ==="

for size in ${BOOKERLY_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="bookerly_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Bookerly/Bookerly-${style}.ttf"
    python fontconvert.py $font_name $size $font_path --2bit --group bookerly --binary-out $BINDIR/${font_name}.fontbin
  done
done

for size in ${NOTOSANS_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notosans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    python fontconvert.py $font_name $size $font_path --2bit --group notosans --binary-out $BINDIR/${font_name}.fontbin
  done
done

for size in ${OPENDYSLEXIC_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="opendyslexic_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/OpenDyslexic/OpenDyslexic-${style}.otf"
    python fontconvert.py $font_name $size $font_path --2bit --group opendyslexic --binary-out $BINDIR/${font_name}.fontbin
  done
done

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
    python fontconvert.py $font_name $size $font_path --group ui --binary-out $BINDIR/${font_name}.fontbin
  done
done

# notosans_8_regular (small UI font, 1-bit)
python fontconvert.py notosans_8_regular 8 ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf --group ui --binary-out $BINDIR/notosans_8_regular.fontbin

# ── Pack all font binaries into partition image ──────────────────────────────
OUTPUT="../../../fontdata.bin"
echo ""
echo "=== Packing font partition image ==="
python fontconvert.py --pack-fonts $BINDIR $OUTPUT
