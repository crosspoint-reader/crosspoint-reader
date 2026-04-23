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
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

for size in ${NOTOSANS_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notosans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum > $output_path
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
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path > $output_path
    echo "Generated $output_path"
  done
done

python fontconvert.py notosans_8_regular 8 ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf > ../builtinFonts/notosans_8_regular.h

# IPA font — Doulos SIL Regular, 16pt, IPA codepoints only
IPA_SOURCE="../builtinFonts/source/DoulosSIL/DoulosSIL-Regular.ttf"
IPA_STRIPPED="/tmp/doulos_sil_ipa_stripped.ttf"
IPA_UNICODES="U+0250-02AF,U+02B0-02FF,U+1D00-1D7F,U+1D80-1DBF"

pyftsubset "$IPA_SOURCE" \
  --unicodes="$IPA_UNICODES" \
  --output-file="$IPA_STRIPPED" \
  --no-layout-closure \
  --drop-tables+=GSUB,GPOS,GDEF,Silt

python fontconvert.py ipa_16_regular 16 "$IPA_STRIPPED" \
  --2bit --compress \
  --no-default-intervals \
  --additional-intervals 0x0250,0x02AF \
  --additional-intervals 0x02B0,0x02FF \
  --additional-intervals 0x1D00,0x1D7F \
  --additional-intervals 0x1D80,0x1DBF \
  > ../builtinFonts/ipa_16_regular.h

echo "Generated ipa_16_regular.h"
rm -f "$IPA_STRIPPED"

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
