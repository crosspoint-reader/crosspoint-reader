#!/bin/bash

set -e

# build_cjk_intervals() below uses a nameref (`local -n`), which needs bash
# 4.3+. macOS ships 3.2 on PATH by default (license reasons) -- install a
# newer bash (e.g. `brew install bash`) and invoke that instead if this trips.
if ((BASH_VERSINFO[0] < 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] < 3))); then
  echo "This script requires bash 4.3+ (found ${BASH_VERSION}). On macOS: brew install bash." >&2
  exit 1
fi

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
NOTOSERIF_FONT_SIZES=(12 14 16 18)
NOTOSANS_FONT_SIZES=(12 14 16 18)

for size in ${NOTOSERIF_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notoserif_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSerif/NotoSerif-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum --zopfli > $output_path
    echo "Generated $output_path"
  done
done

for size in ${NOTOSANS_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notosans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum --zopfli > $output_path
    echo "Generated $output_path"
  done
done

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

# Arabic glyphs for UI text (menus, file browser titles). The built-in fonts
# must cover the *output* of MiniBidi's do_shape() — contextual presentation
# forms — not base letters, or shaped UI text silently drops glyphs.
# Curated for firmware-size budget: core Arabic (Presentation Forms-B,
# incl. the Lam-Alef ligature forms) plus the Farsi/Urdu extra letters'
# Presentation Forms-A blocks, the few characters shaping leaves at their
# base codepoint, Arabic punctuation, and both digit sets. No harakat and
# no Sindhi/Pashto/Kurdish forms — book text gets those from SD-card fonts.
ARABIC_INTERVALS=(
  --additional-intervals 0x060C,0x060C  # Arabic comma
  --additional-intervals 0x061B,0x061B  # Arabic semicolon
  --additional-intervals 0x061F,0x061F  # Arabic question mark
  --additional-intervals 0x0621,0x0621  # hamza (non-joining, never shaped)
  --additional-intervals 0x0640,0x0640  # tatweel
  --additional-intervals 0x0654,0x0654  # Persian/Urdu ezafe hamza, Arabic hamza carriers
  --additional-intervals 0x0660,0x0669  # Arabic-Indic digits
  --additional-intervals 0x06BA,0x06BA  # noon ghunna base (initial/medial keep base cp)
  --additional-intervals 0x06D4,0x06D4  # Urdu full stop
  --additional-intervals 0x06D5,0x06D5  # ae (isolated; Kurdish/Uyghur/Ottoman) — has no presentation form
  --additional-intervals 0x06F0,0x06F9  # extended Arabic-Indic digits (Farsi/Urdu)
  --additional-intervals 0xFB56,0xFB59  # peh (Farsi)
  --additional-intervals 0xFB66,0xFB69  # tteh (Urdu)
  --additional-intervals 0xFB7A,0xFB7D  # tcheh (Farsi)
  --additional-intervals 0xFB88,0xFB95  # ddal, jeh, rreh (Urdu), keheh, gaf (Farsi/Urdu)
  --additional-intervals 0xFB9E,0xFB9F  # noon ghunna isolated/final (Urdu)
  --additional-intervals 0xFBA6,0xFBB1  # heh goal, heh doachashmee, yeh barree(+hamza) (Urdu)
  --additional-intervals 0xFBFC,0xFBFF  # farsi yeh (Farsi/Urdu)
  --additional-intervals 0xFE80,0xFEFC  # Presentation Forms-B: core Arabic + Lam-Alef
)

# CJK glyphs for the language picker only (e.g. "繁體中文"), NOT for CJK UI
# text in general. A full CJK UI font (menus, settings, dialogs — every
# STR_* string) costs ~120KB of flash per weight for a single language's
# ~440-codepoint UI subset; that's a firmware-size tax every device pays
# regardless of language, so it isn't merged in here at all. Instead, only
# the characters appearing in some language's own _language_name are baked
# in — a handful of codepoints, ~1KB total — so every language's entry in
# the picker is legible even before that language is selected. Once a CJK
# language is actually active, its menu text is rendered via an SD-card CJK
# fallback font (see GfxRenderer::setFallbackFont / SdCardFontSystem), not
# via the built-in font. Scans every translation file, not just
# chinese-traditional.yaml, so adding another CJK language later only
# requires adding its _language_name here — never a fresh flash budget
# fight. Computed separately per base font (not shared) because
# Ubuntu-Regular.ttf and NotoSans-Regular.ttf disagree on ~2000 non-ASCII
# codepoints overall; reusing one list for both would silently miss glyphs
# in whichever font wasn't used to compute it.
#
# --codepoint-range restricts this to Han + Kana: some other language's own
# name also has codepoints missing from Ubuntu-Regular.ttf's raw cmap (e.g.
# base Arabic/Persian letters — a separate, pre-existing gap; the Arabic UI
# path only ever renders MiniBidi's shaped presentation forms, curated
# above, never base letters), and this scan must not silently absorb those
# into a "CJK" glyph budget. Extend with a Hangul range (0xAC00-0xD7AF) if
# a Korean translation is ever added.
#
# Written to real temp files rather than a `while read < <(cmd)` process
# substitution: bash doesn't fail the enclosing `set -e` script when the
# command feeding a process substitution errors, so a broken
# gen_cjk_ui_intervals.py run would silently produce zero intervals here —
# and the resulting UI fonts would build "successfully" with no Han glyphs.
# `python ... > file` is a plain redirected command, so its exit status
# does trip `set -e`.
cjk_intervals_tmp="$(mktemp)"
build_cjk_intervals() {
  local exclude_cmap="$1"
  local -n out_array="$2"
  python gen_cjk_ui_intervals.py \
    --yaml ../../I18n/translations/*.yaml \
    --only-keys _language_name \
    --codepoint-range 0x4E00,0x9FFF --codepoint-range 0x3040,0x30FF \
    --exclude-cmap "$exclude_cmap" > "$cjk_intervals_tmp"
  out_array=()
  local pair
  while IFS= read -r pair; do
    out_array+=(--additional-intervals "$pair")
  done < "$cjk_intervals_tmp"
}
build_cjk_intervals ../builtinFonts/source/Ubuntu/Ubuntu-Regular.ttf CJK_INTERVALS_UBUNTU
build_cjk_intervals ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf CJK_INTERVALS_NOTOSANS
rm -f "$cjk_intervals_tmp"

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
    hebrew_path="../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-${style}.ttf"
    arabic_path="../builtinFonts/source/NotoSansArabic/NotoSansArabic-${style}.ttf"
    cjk_path="../builtinFonts/source/NotoSansCJKtcUiSubset/NotoSansCJKtc-${style}.otf"
    # Ubuntu lacks the Latin Extended Additional block (U+1EA0-U+1EF9) used for
    # Vietnamese tone marks. Append a Vietnamese-only Ubuntu cut so those glyphs
    # are filled from it while every glyph Ubuntu already has stays unchanged
    # (fontstack is ordered by descending priority).
    viet_path="../builtinFonts/source/Ubuntu/Ubuntu-Vietnamese-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $hebrew_path $arabic_path $viet_path $cjk_path \
      --additional-intervals 0x05D0,0x05EA "${ARABIC_INTERVALS[@]}" "${CJK_INTERVALS_UBUNTU[@]}" > $output_path
    echo "Generated $output_path"
  done
done

python fontconvert.py notosans_8_regular 8 \
  ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf \
  ../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-Regular.ttf \
  ../builtinFonts/source/NotoSansArabic/NotoSansArabic-Regular.ttf \
  ../builtinFonts/source/NotoSansCJKtcUiSubset/NotoSansCJKtc-Regular.otf \
  --additional-intervals 0x05D0,0x05EA "${ARABIC_INTERVALS[@]}" "${CJK_INTERVALS_NOTOSANS[@]}" > ../builtinFonts/notosans_8_regular.h

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
