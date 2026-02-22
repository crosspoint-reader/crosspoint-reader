#!/bin/bash

set -e

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
BOOKERLY_FONT_SIZES=(12 14 16)
NOTOSANS_FONT_SIZES=(12 14 16)


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
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path > $output_path
    echo "Generated $output_path"
  done
done

python fontconvert.py notosans_8_regular 8 ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf > ../builtinFonts/notosans_8_regular.h

# CJK fonts (compressed, 2-bit, frequency-grouped)
CJK_FONT="../builtinFonts/source/NotoSansCJK/NotoSansCJKsc-Regular.otf"
CJK_FREQ="data/cjk_frequency.tsv"
CJK_INTERVALS="0x3000,0x303F 0x3040,0x309F 0x30A0,0x30FF 0x4E00,0x9FFF 0xAC00,0xD7AF 0xFF00,0xFFEF"

if [ -f "$CJK_FONT" ]; then
  for size in 14; do
    font_name="notosanscjk_${size}_regular"
    output_path="../builtinFonts/${font_name}.h"
    interval_args=""
    for interval in $CJK_INTERVALS; do
      interval_args="$interval_args --additional-intervals $interval"
    done
    python fontconvert.py $font_name $size "$CJK_FONT" --2bit --compress \
      --frequency-table "$CJK_FREQ" --group-size 64 --pin-groups 0 \
      --non-pinned-group-size 64 \
      --max-cjk-ideographs 0 --max-hangul 2350 \
      $interval_args > $output_path
    echo "Generated $output_path"
  done
else
  echo "Skipping CJK fonts: $CJK_FONT not found"
fi

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
