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
    # Ubuntu lacks the Latin Extended Additional block (U+1EA0-U+1EF9) used for
    # Vietnamese tone marks. Append a Vietnamese-only Ubuntu cut so those glyphs
    # are filled from it while every glyph Ubuntu already has stays unchanged
    # (fontstack is ordered by descending priority).
    viet_path="../builtinFonts/source/Ubuntu/Ubuntu-Vietnamese-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $hebrew_path $viet_path \
      --additional-intervals 0x05D0,0x05EA > $output_path
    echo "Generated $output_path"
  done
done

python fontconvert.py notosans_8_regular 8 \
  ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf \
  ../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-Regular.ttf \
  --additional-intervals 0x05D0,0x05EA > ../builtinFonts/notosans_8_regular.h

# ensure_stripped_font <display_name> <dir> <stripped_ttf> <zip_url> <ttf_filename_in_zip> <unicodes>
# Downloads <zip_url>, extracts <ttf_filename_in_zip>, subsets to <unicodes>, writes <stripped_ttf>.
# No-ops if <stripped_ttf> already exists.
ensure_stripped_font() {
  local NAME="$1"
  local DIR="$2"
  local STRIPPED="$3"
  local ZIP_URL="$4"
  local TTF_NAME="$5"
  local UNICODES="$6"

  [ -f "$STRIPPED" ] && return 0

  echo "${NAME}: stripped TTF not found; downloading to generate..."
  mkdir -p "$DIR"
  local TEMP_ZIP TEMP_DIR FULL FOUND_TTF
  TEMP_ZIP=$(mktemp /tmp/fontdl_XXXXXX.zip)
  TEMP_DIR=$(mktemp -d /tmp/fontdl_XXXXXX)

  curl -L -o "$TEMP_ZIP" "$ZIP_URL"
  unzip -q -o "$TEMP_ZIP" -d "$TEMP_DIR"

  FOUND_TTF=$(find "$TEMP_DIR" -name "$TTF_NAME" | head -n 1)
  if [ -z "$FOUND_TTF" ]; then
    echo "Error: $TTF_NAME not found in $ZIP_URL"
    rm -rf "$TEMP_ZIP" "$TEMP_DIR"
    exit 1
  fi

  FULL="${DIR}/${TTF_NAME}"
  cp "$FOUND_TTF" "$FULL"
  rm -rf "$TEMP_ZIP" "$TEMP_DIR"

  echo "${NAME}: stripping to ${UNICODES}..."
  pyftsubset "$FULL" --unicodes="$UNICODES" --output-file="$STRIPPED"
  rm -f "$FULL"
  echo "${NAME}: stripped font written to $STRIPPED"
}

# dict font — Doulos SIL Regular, 16pt, phonetic/dict codepoints
# Full TTF ~868 KB; stripped to IPA/phonetic ranges ~188 KB. Generated on demand (requires fonttools).
SIL_DIR="../builtinFonts/source/DoulosSIL"
SIL_STRIPPED="${SIL_DIR}/DoulosSIL-Regular-stripped.ttf"
ensure_stripped_font "DoulosSIL" "$SIL_DIR" "$SIL_STRIPPED" \
  "https://github.com/silnrsi/font-doulos/releases/download/v7.000/DoulosSIL-7.000.zip" \
  "DoulosSIL-Regular.ttf" \
  "U+0220-03FF,U+1D00-1FFF,U+20D0-20FF,U+2153-2154,U+2192,U+221A,U+266D-266F,U+261E"
DICT_SOURCE="$SIL_STRIPPED"
NOTOSERIF_SOURCE="../builtinFonts/source/NotoSerif/NotoSerif-Regular.ttf"

# Symbols font — Noto Sans Symbols 2 Regular.
# Full TTF ~650 KB; only U+261E (☞) is used so stripped to a single glyph. Generated on demand.
SYMBOLS_DIR="../builtinFonts/source/NotoSansSymbols2"
SYMBOLS_STRIPPED="${SYMBOLS_DIR}/NotoSansSymbols2-Regular-stripped.ttf"
ensure_stripped_font "NotoSansSymbols2" "$SYMBOLS_DIR" "$SYMBOLS_STRIPPED" \
  "https://github.com/notofonts/symbols/releases/download/NotoSansSymbols2-v2.008/NotoSansSymbols2-v2.008.zip" \
  "NotoSansSymbols2-Regular.ttf" \
  "U+261E"
SYMBOLS_SOURCE="$SYMBOLS_STRIPPED"

python fontconvert.py dict_16_regular 16 "$DICT_SOURCE" "$SYMBOLS_SOURCE" "$NOTOSERIF_SOURCE" \
  --2bit --compress \
  --no-default-intervals \
  --additional-intervals 0x0220,0x02FF \
  --additional-intervals 0x0300,0x036F \
  --additional-intervals 0x0370,0x03FF \
  --additional-intervals 0x1D00,0x1DBF \
  --additional-intervals 0x1DC0,0x1DFF \
  --additional-intervals 0x1E00,0x1FFF \
  --additional-intervals 0x20D0,0x20FF \
  --additional-intervals 0x2153,0x2154 \
  --additional-intervals 0x2192,0x2192 \
  --additional-intervals 0x221A,0x221A \
  --additional-intervals 0x266D,0x266F \
  --additional-intervals 0x261E,0x261E \
  > ../builtinFonts/dict_16_regular.h

echo "Generated dict_16_regular.h"

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
