#!/bin/bash

set -e

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
READER_FONT_SIZES=(12 14 16 18)

for size in ${READER_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="charein_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/ChareInk/ChareInk-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress > $output_path
    echo "Generated $output_path"
  done
done

for size in ${READER_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="lexenddeca_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    if [ "$style" = "BoldItalic" ]; then
      font_path="../builtinFonts/source/LexendDeca/LexendDeca-ItalicBold.ttf"
    else
      font_path="../builtinFonts/source/LexendDeca/LexendDeca-${style}.ttf"
    fi
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

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
