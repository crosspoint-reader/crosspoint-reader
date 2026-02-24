#!/bin/bash

set -e

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
BOOKERLY_FONT_SIZES=(12 14 16 18)
NOTOSANS_FONT_SIZES=(12 14 16 18)
OPENDYSLEXIC_FONT_SIZES=(8 10 12 14)

for size in ${BOOKERLY_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="bookerly_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Bookerly/Bookerly-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --force-autohint > $output_path
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

for size in ${OPENDYSLEXIC_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="opendyslexic_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/OpenDyslexic/OpenDyslexic-${style}.otf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress > $output_path
    echo "Generated $output_path"
  done
done

# UI fonts

UI_FONT_STYLES=("Regular" "Bold")

# For 10pt UI font, we fallback to 10pt NotoSans at conversion time
# Therefore, we need to specify Ubuntu and NotoSans here
size=10
for style in ${UI_FONT_STYLES[@]}; do
  font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
  font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
  font_fallback_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
  output_path="../builtinFonts/${font_name}.h"
  python fontconvert.py $font_name $size $font_path $font_fallback_path > $output_path
  echo "Generated $output_path"
done

# For 12pt UI font, we fallback to 12pt NotoSans at runtime
size=12
for style in ${UI_FONT_STYLES[@]}; do
  font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
  font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
  output_path="../builtinFonts/${font_name}.h"
  python fontconvert.py $font_name $size $font_path > $output_path
  echo "Generated $output_path"
done

python fontconvert.py notosans_8_regular 8 ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf > ../builtinFonts/notosans_8_regular.h

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
