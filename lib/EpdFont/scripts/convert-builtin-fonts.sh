#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/font-sizes.sh"
cd "$SCRIPT_DIR"

for size in ${BOOKERLY_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="bookerly_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Bookerly/Bookerly-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --zopfli > $output_path
    echo "Generated $output_path"
  done
done

# The phonetic alphabet a dictionary needs: IPA Extensions, the stress and
# length marks from Spacing Modifier Letters, and the three Greek letters IPA
# borrows.
#
# Only ONE face gets them, notosans_14, because the dictionary viewer pins
# itself to that font (see DICTIONARY_FONT_ID in
# src/activities/reader/DictionaryDefinitionActivity.h) and a definition is the
# only text that needs the range. Giving it to all twenty Noto faces cost 124 KB
# of flash; one size costs 26 KB. Noto Sans is also the only built-in reader
# family whose source actually HAS the glyphs -- Bookerly is missing 9 of the 20
# codepoints a real dictionary uses, which is why every other font relies on the
# lookalike substitution in lib/EpdFont/GlyphFallback.h instead.
# The whole IPA Extensions block, but only the handful of Spacing Modifier
# Letters a transcription actually uses: taking that block whole cost 31 KB more
# across the family for ~60 codepoints no dictionary writes.
NOTOSANS_IPA_SIZE=14
NOTOSANS_IPA_INTERVALS=(
  "0x0250,0x02AF"  # IPA Extensions
  "0x02B0,0x02B7"  # superscript modifiers, aspiration and friends
  "0x02BC,0x02BC"  # modifier apostrophe
  "0x02C8,0x02D1"  # stress marks and length marks
  "0x02E1,0x02E1"  # modifier l
  "0x03B2,0x03B2"  # beta
  "0x03B8,0x03B8"  # theta
  "0x03C7,0x03C7"  # chi
)

for size in ${NOTOSANS_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notosans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    cmd=(python fontconvert.py $font_name $size $font_path --2bit --compress --zopfli)
    if [ "$size" = "$NOTOSANS_IPA_SIZE" ]; then
      for interval in "${NOTOSANS_IPA_INTERVALS[@]}"; do
        cmd+=(--additional-intervals "$interval")
      done
    fi
    "${cmd[@]}" > $output_path
    echo "Generated $output_path"
  done
done

UI_LANG_INTERVALS=(
  "0x0000,0x007F"
  "0x0080,0x00FF"
  "0x0100,0x017F"
  "0x01A0,0x01A1"
  "0x01AF,0x01B0"
  "0x01C4,0x021F"
  "0x0300,0x036F"
  "0x0400,0x04FF"
  "0x1EA0,0x1EF9"
  "0x2010,0x206F"
  "0x20A0,0x20CF"
  "0xFB00,0xFB06"
  "0xFFFD,0xFFFD"
)

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="inter_ui_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    inter_path="../builtinFonts/source/Inter/Inter-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"

    cmd=(python fontconvert.py "$font_name" "$size" "$inter_path")
    for interval in "${UI_LANG_INTERVALS[@]}"; do
      cmd+=(--additional-intervals "$interval")
    done
    "${cmd[@]}" > "$output_path"

    echo "Generated $output_path"
  done
done

python fontconvert.py notosans_8_regular 8 ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf > ../builtinFonts/notosans_8_regular.h

# The CJK UI faces hold each CJK translation's characters that the UI faces above lack, so they
# come after them. The script dedupes and verifies by itself; the pass below repeats that over the
# whole set, which is harmless.
python build_cjk_ui_fonts.py

echo ""
echo "Deduplicating tables shared between faces..."
# fontconvert.py writes each face independently, so every size of a family repeats the tables
# that describe the TYPEFACE rather than the size -- codepoint coverage, kern class assignment,
# the ligature set. Only the kern values scale with size. This pass runs over the finished set
# and keeps one copy of each; it never computes a table, so it cannot change any bytes.
# Idempotent, so re-running the conversion is safe.
# Snapshot first so the dedup can be PROVEN byte-equivalent rather than assumed. The verifier
# is a separate program that re-reads both trees from disk: one that shared the transformer's
# parsing would agree with it about a misparse.
PREDEDUPE="$(mktemp -d)"
cp ../builtinFonts/*.h "$PREDEDUPE/"
python dedupe_font_tables.py ../builtinFonts/
python verify_font_dedup.py ../builtinFonts/ "$PREDEDUPE"
rm -rf "$PREDEDUPE"

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
