#!/bin/bash

set -e

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
BOOKERLY_FONT_SIZES=(12 14 16 18)
NOTOSANS_FONT_SIZES=(12 14 16 18)
# Inter 8 is generated at the end (shared by SMALL_FONT_ID and INTER_8_FONT_ID)
INTER_FONT_SIZES=(14 16 18)
DETERMINATION_SANS_FONT_SIZES=(8)
for size in ${BOOKERLY_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="bookerly_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Bookerly/Bookerly-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python3 fontconvert.py $font_name $size $font_path --2bit --compress > $output_path
    echo "Generated $output_path"
  done
done

for size in ${NOTOSANS_FONT_SIZES[@]}; do
  font_name="notosans_${size}_regular"
  font_path="../builtinFonts/source/NotoSans/NotoSans-Regular.ttf"
  output_path="../builtinFonts/${font_name}.h"
  python3 fontconvert.py $font_name $size $font_path --2bit --compress > $output_path
  echo "Generated $output_path"
  
  font_name="notosans_${size}_bold"
  font_path="../builtinFonts/source/NotoSans/NotoSans-Bold.ttf"
  output_path="../builtinFonts/${font_name}.h"
  python3 fontconvert.py $font_name $size $font_path --2bit --compress > $output_path
  echo "Generated $output_path"
  
  font_name="notosans_${size}_italic"
  font_path="../builtinFonts/source/NotoSans/NotoSans-Italic.ttf"
  output_path="../builtinFonts/${font_name}.h"
  python3 fontconvert.py $font_name $size $font_path --2bit --compress > $output_path
  echo "Generated $output_path"

  font_name="notosans_${size}_bolditalic"
  font_path="../builtinFonts/source/NotoSans/NotoSans-BoldItalic.ttf"
  output_path="../builtinFonts/${font_name}.h"
  python3 fontconvert.py $font_name $size $font_path --2bit --compress > $output_path
  echo "Generated $output_path"
done

for size in ${DETERMINATION_SANS_FONT_SIZES[@]}; do
  font_name="determinationsans_${size}_regular"
  font_path="../builtinFonts/source/DeterminationSans.otf"
  output_path="../builtinFonts/${font_name}.h"
  python3 fontconvert.py $font_name $size $font_path > $output_path
  echo "Generated $output_path"
done

for size in ${INTER_FONT_SIZES[@]}; do
  font_name="inter_${size}_regular"
  font_path="../builtinFonts/source/Inter/Inter-Regular.ttf"
  output_path="../builtinFonts/${font_name}.h"
  python3 fontconvert.py $font_name $size $font_path --2bit --compress > $output_path
  echo "Generated $output_path"
  
  font_name="inter_${size}_bold"
  font_path="../builtinFonts/source/Inter/Inter-Bold.ttf"
  output_path="../builtinFonts/${font_name}.h"
  python3 fontconvert.py $font_name $size $font_path --2bit --compress > $output_path
  echo "Generated $output_path"
done

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="inter_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    if [ "$style" = "Regular" ]; then
      font_path="../builtinFonts/source/Inter/Inter-Regular.ttf"
    else
      font_path="../builtinFonts/source/Inter/Inter-Bold.ttf"
    fi
    output_path="../builtinFonts/${font_name}.h"
    python3 fontconvert.py $font_name $size $font_path > $output_path
    echo "Generated $output_path"
  done
done

python3 fontconvert.py inter_8_regular 8 ../builtinFonts/source/Inter/Inter-Regular.ttf > ../builtinFonts/inter_8_regular.h

echo ""
echo "Running compression verification..."
python3 verify_compression.py ../builtinFonts/
