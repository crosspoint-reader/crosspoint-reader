#!/bin/bash

set -e

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
BOOKERLY_FONT_SIZES=(12 14 16 18)
NOTOSANSTC_FONT_SIZES=(12 14 16 18)
for size in ${BOOKERLY_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="bookerly_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Bookerly/Bookerly-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python3 fontconvert.py $font_name $size $font_path --2bit --compress > $output_path
    echo "Generated $output_path"
  done
done

for size in ${NOTOSANSTC_FONT_SIZES[@]}; do
  # NotoSansTC only has Regular and Bold styles
  font_name="notosanstc_${size}_regular"
  font_path="../builtinFonts/source/NotoSansTC/NotoSansTC-Regular.ttf"
  output_path="../builtinFonts/${font_name}.h"
  python3 fontconvert.py $font_name $size $font_path --2bit --compress > $output_path
  echo "Generated $output_path"
  
  font_name="notosanstc_${size}_bold"
  font_path="../builtinFonts/source/NotoSansTC/NotoSansTC-Bold.ttf"
  output_path="../builtinFonts/${font_name}.h"
  python3 fontconvert.py $font_name $size $font_path --2bit --compress > $output_path
  echo "Generated $output_path"
  
  # Create italic and bolditalic by copying regular and bold
  cp "../builtinFonts/notosanstc_${size}_regular.h" "../builtinFonts/notosanstc_${size}_italic.h"
  sed -i '' 's/notosanstc_'"${size}"'_regular/notosanstc_'"${size}"'_italic/g' "../builtinFonts/notosanstc_${size}_italic.h"
  echo "Generated ../builtinFonts/notosanstc_${size}_italic.h (copied from regular)"
  
  cp "../builtinFonts/notosanstc_${size}_bold.h" "../builtinFonts/notosanstc_${size}_bolditalic.h"
  sed -i '' 's/notosanstc_'"${size}"'_bold/notosanstc_'"${size}"'_bolditalic/g' "../builtinFonts/notosanstc_${size}_bolditalic.h"
  echo "Generated ../builtinFonts/notosanstc_${size}_bolditalic.h (copied from bold)"
done

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python3 fontconvert.py $font_name $size $font_path > $output_path
    echo "Generated $output_path"
  done
done

python3 fontconvert.py notosanstc_8_regular 8 ../builtinFonts/source/NotoSansTC/NotoSansTC-Regular.ttf > ../builtinFonts/notosanstc_8_regular.h

echo ""
echo "Running compression verification..."
python3 verify_compression.py ../builtinFonts/
