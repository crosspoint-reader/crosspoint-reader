#!/bin/bash

set -e

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
NOTOSERIF_FONT_SIZES=(12 14 16 18)
NOTOSANS_FONT_SIZES=(12 14 16 18)

for size in ${NOTOSERIF_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notoserif_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSerif/NotoSerif-${style}.ttf"
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

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
    hebrew_path="../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $hebrew_path \
      --additional-intervals 0x05D0,0x05EA > $output_path
    echo "Generated $output_path"
  done
done

python fontconvert.py notosans_8_regular 8 \
  ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf \
  ../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-Regular.ttf \
  --additional-intervals 0x05D0,0x05EA > ../builtinFonts/notosans_8_regular.h

# IPA font — Doulos SIL Regular, 16pt, IPA codepoints only
IPA_SOURCE="../builtinFonts/source/DoulosSIL/DoulosSIL-Regular.ttf"
IPA_STRIPPED="/tmp/doulos_sil_ipa_stripped.ttf"
IPA_UNICODES="U+00E6,U+00F0,U+00F8,U+0127,U+014B,U+0153,U+03B2,U+03B8,U+03C7,U+0250-02AF,U+02B0-02FF,U+0300-036F,U+1D00-1D7F,U+1D80-1DBF,U+1DC0-1DFF,U+20D0-20FF"

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
  --additional-intervals 0x0300,0x036F \
  --additional-intervals 0x1D00,0x1D7F \
  --additional-intervals 0x1D80,0x1DBF \
  --additional-intervals 0x1DC0,0x1DFF \
  --additional-intervals 0x20D0,0x20FF \
  --additional-intervals 0x00E6,0x00E6 \
  --additional-intervals 0x00F0,0x00F0 \
  --additional-intervals 0x00F8,0x00F8 \
  --additional-intervals 0x0127,0x0127 \
  --additional-intervals 0x014B,0x014B \
  --additional-intervals 0x0153,0x0153 \
  --additional-intervals 0x03B2,0x03B2 \
  --additional-intervals 0x03B8,0x03B8 \
  --additional-intervals 0x03C7,0x03C7 \
  > ../builtinFonts/ipa_16_regular.h

echo "Generated ipa_16_regular.h"
rm -f "$IPA_STRIPPED"

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
