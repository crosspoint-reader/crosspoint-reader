#include "CjkUiFonts.h"

#include <EpdFont.h>
#include <FallbackFaceProvider.h>
#include <Logging.h>
#include <Utf8.h>
#include <builtinFonts/cjk_ui_ja_10.h>
#include <builtinFonts/cjk_ui_ja_12.h>
#include <builtinFonts/cjk_ui_ko_10.h>
#include <builtinFonts/cjk_ui_ko_12.h>
#include <builtinFonts/cjk_ui_zh_10.h>
#include <builtinFonts/cjk_ui_zh_12.h>

#include <cstddef>

namespace {

// CJK_UI_FONT_SIZES in lib/EpdFont/scripts/font-sizes.sh, ascending; a size added there needs its
// faces added to every row below.
constexpr uint8_t SIZES[] = {10, 12};
constexpr size_t SIZE_COUNT = sizeof(SIZES) / sizeof(SIZES[0]);

struct LanguageFaces {
  Language language;
  const EpdFontData* faces[SIZE_COUNT];
};

// In the order searched for a non-CJK UI language. The faces and their regions are listed in
// lib/EpdFont/scripts/ui_font_coverage.py (CJK_UI_LANGUAGES).
const LanguageFaces LANGUAGES[] = {
    {Language::ZH, {&cjk_ui_zh_10, &cjk_ui_zh_12}},
    {Language::JA, {&cjk_ui_ja_10, &cjk_ui_ja_12}},
    {Language::KO, {&cjk_ui_ko_10, &cjk_ui_ko_12}},
};
constexpr size_t LANGUAGE_COUNT = sizeof(LANGUAGES) / sizeof(LANGUAGES[0]);
static_assert(LANGUAGE_COUNT <= FallbackFaceProvider::MAX_FACES, "one provider face per CJK language");

FallbackFaceProvider providers[SIZE_COUNT];

// WHITE SQUARE, which every CJK UI face carries (CJK_UI_STAND_IN in ui_font_coverage.py): what a
// CJK character no face has draws as. A book title the faces do not cover then shows boxes where
// its characters are, instead of the one or two it shares with a translation standing alone.
constexpr uint32_t STAND_IN = 0x25A1;

// The characters the CJK faces answer for. Not the ideographic space, which every face carries
// anyway (a box is no stand-in for a space), and not the Hangul fillers, which are default-ignorable
// and must never draw ink.
bool isCjkCharacter(const uint32_t cp) { return utf8IsCjkBreakable(cp) && cp != 0x3000 && !utf8IsDefaultIgnorable(cp); }

// The UI faces main.cpp attaches: notosans_8 and Inter 10/12/14 in regular and bold.
constexpr size_t MAX_ATTACHED = 7;
EpdFontData attached[MAX_ATTACHED];
size_t attachedCount = 0;

size_t sizeIndexFor(const uint8_t points) {
  size_t index = 0;
  for (size_t i = 0; i < SIZE_COUNT; ++i) {
    if (SIZES[i] <= points) index = i;
  }
  return index;
}

}  // namespace

void CjkUiFonts::attach(EpdFont& font, const uint8_t points) {
  const EpdFontData* data = font.data;
  // A provider already there is an SD or TTF font's own; a compressed or 2-bit face could not draw
  // a 1-bit glyph from a flat bitmap.
  if (data->provider || data->groups || data->is2Bit) {
    LOG_ERR("CJK", "UI face %u pt cannot take CJK fallback glyphs", points);
    return;
  }
  if (attachedCount == MAX_ATTACHED) {
    LOG_ERR("CJK", "more than %u UI faces attached", static_cast<unsigned>(MAX_ATTACHED));
    return;
  }
  EpdFontData& copy = attached[attachedCount++];
  copy = *data;
  copy.provider = &providers[sizeIndexFor(points)];
  font.data = &copy;
}

void CjkUiFonts::setLanguage(const Language language) {
  for (size_t size = 0; size < SIZE_COUNT; ++size) {
    const EpdFontData* order[LANGUAGE_COUNT];
    size_t count = 0;
    for (const LanguageFaces& row : LANGUAGES) {
      if (row.language == language) order[count++] = row.faces[size];
    }
    for (const LanguageFaces& row : LANGUAGES) {
      if (row.language != language) order[count++] = row.faces[size];
    }
    providers[size].setFaces(order, static_cast<uint8_t>(count));
    providers[size].setStandIn(STAND_IN, &isCjkCharacter);
  }
}
