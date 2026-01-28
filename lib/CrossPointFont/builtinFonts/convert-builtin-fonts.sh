#!/bin/bash

set -e

cd "$(dirname "$0")"

BOOKERLY_FONT_SIZES=(12 14 16 18)
NOTOSANS_FONT_SIZES=(12 14 16 18)
OPENDYSLEXIC_FONT_SIZES=(8 10 12 14)

for size in ${BOOKERLY_FONT_SIZES[@]}; do
  font_name="bookerly_${size}"
  font_path_prefix="./source/Bookerly/Bookerly-"
  output_path="./${font_name}.h"
  ../fontconvert/fontconvert "${font_path_prefix}Regular.ttf" -b "${font_path_prefix}Bold.ttf" -i "${font_path_prefix}Italic.ttf" -bi "${font_path_prefix}BoldItalic.ttf" -o $output_path -p $size
done

for size in ${NOTOSANS_FONT_SIZES[@]}; do
  font_name="notosans_${size}"
  font_path_prefix="./source/NotoSans/NotoSans-"
  output_path="./${font_name}.h"
  ../fontconvert/fontconvert "${font_path_prefix}Regular.ttf" -b "${font_path_prefix}Bold.ttf" -i "${font_path_prefix}Italic.ttf" -bi "${font_path_prefix}BoldItalic.ttf" -o $output_path -p $size
done

for size in ${OPENDYSLEXIC_FONT_SIZES[@]}; do
  font_name="opendyslexic_${size}"
  font_path_prefix="./source/OpenDyslexic/OpenDyslexic-"
  output_path="./${font_name}.h"
  ../fontconvert/fontconvert "${font_path_prefix}Regular.otf" -b "${font_path_prefix}Bold.otf" -i "${font_path_prefix}Italic.otf" -bi "${font_path_prefix}BoldItalic.otf" -o $output_path -p $size
done

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

for size in ${UI_FONT_SIZES[@]}; do
  font_name="ubuntu_${size}"
  font_path_prefix="./source/Ubuntu/Ubuntu-"
  output_path="./${font_name}.h"
  ../fontconvert/fontconvert "${font_path_prefix}Regular.ttf" -b "${font_path_prefix}Bold.ttf" -o $output_path -p $size
done

../fontconvert/fontconvert ./source/NotoSans/NotoSans-Regular.ttf -o ./notosans_8.h -p 8
