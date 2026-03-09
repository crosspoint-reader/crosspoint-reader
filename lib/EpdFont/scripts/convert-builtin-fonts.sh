#!/bin/bash

set -e

cd "$(dirname "$0")"

THAI_INTERVAL="0x0E00,0x0E7F"

# Thai fallback font selection: Thai has no italic, so Regular is used for
# Regular/Italic and Bold for Bold/BoldItalic.
thai_reader_font() {
  local style="$1"
  local thai_weight="Regular"
  if [[ "$style" == "Bold" || "$style" == "BoldItalic" ]]; then
    thai_weight="Bold"
  fi
  echo "../builtinFonts/source/NotoSansThai/NotoSansThai-${thai_weight}.ttf"
}

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
BOOKERLY_FONT_SIZES=(12 14 16 18)
NOTOSANS_FONT_SIZES=(12 14 16 18)
OPENDYSLEXIC_FONT_SIZES=(8 10 12 14)

for size in ${BOOKERLY_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="bookerly_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Bookerly/Bookerly-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress > $output_path
    echo "Generated $output_path"
  done
done

for size in ${NOTOSANS_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notosans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    thai_font="$(thai_reader_font "$style")"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path "$thai_font" \
      --additional-intervals "$THAI_INTERVAL" --2bit --compress > $output_path
    echo "Generated $output_path"
  done
done

for size in ${OPENDYSLEXIC_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="opendyslexic_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/OpenDyslexic/OpenDyslexic-${style}.otf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress > $output_path
    echo "Generated $output_path"
  done
done

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
    thai_font="../builtinFonts/source/Sarabun/Sarabun-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path "$thai_font" \
      --additional-intervals "$THAI_INTERVAL" > $output_path
    echo "Generated $output_path"
  done
done

python fontconvert.py notosans_8_regular 8 \
  ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf \
  ../builtinFonts/source/NotoSansThai/NotoSansThai-Regular.ttf \
  --additional-intervals "$THAI_INTERVAL" > ../builtinFonts/notosans_8_regular.h

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
