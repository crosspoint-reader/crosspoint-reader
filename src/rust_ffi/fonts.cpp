// Font resolution and text metrics.

#include "CrossPointSettings.h"
#include "fontIds.h"
#include "internal.h"

using rust_ffi::asText;
using rust_ffi::resolveStyle;

namespace {

// Mirrors FontFamily on the Rust side.
enum class Family : uint8_t { NotoSerif = 0, NotoSans = 1, Ui = 2, Small = 3 };

// Returns 0 when this build does not ship the font. Must stay in step with the
// insertFont() calls in main.cpp: OMIT_FONTS (the `slim` env) registers only a
// subset, and an unregistered id makes GfxRenderer log on every draw.
int resolve(const uint8_t family, const uint8_t sizePt) {
  switch (static_cast<Family>(family)) {
    case Family::NotoSerif:
      if (sizePt == 14) return NOTOSERIF_14_FONT_ID;
#ifndef OMIT_FONTS
      if (sizePt == 12) return NOTOSERIF_12_FONT_ID;
      if (sizePt == 16) return NOTOSERIF_16_FONT_ID;
      if (sizePt == 18) return NOTOSERIF_18_FONT_ID;
#endif
      return 0;

    case Family::NotoSans:
#ifndef OMIT_FONTS
      if (sizePt == 12) return NOTOSANS_12_FONT_ID;
      if (sizePt == 14) return NOTOSANS_14_FONT_ID;
      if (sizePt == 16) return NOTOSANS_16_FONT_ID;
      if (sizePt == 18) return NOTOSANS_18_FONT_ID;
#endif
      return 0;

    case Family::Ui:
      if (sizePt == 10) return UI_10_FONT_ID;
      if (sizePt == 12) return UI_12_FONT_ID;
      return 0;

    case Family::Small:
      return SMALL_FONT_ID;
  }
  return 0;
}

}  // namespace

extern "C" {

int32_t cpp_resolve_font(const uint8_t family, const uint8_t sizePt) { return resolve(family, sizePt); }

int32_t cpp_reader_font() { return SETTINGS.getReaderFontId(); }

int32_t cpp_text_width(const int32_t fontId, const uint8_t* text, const uint8_t style) {
  if (!g_rustRendererPtr || !text || fontId == 0) return 0;
  return g_rustRendererPtr->getTextWidth(fontId, asText(text), resolveStyle(style));
}

int32_t cpp_line_height(const int32_t fontId) {
  if (!g_rustRendererPtr || fontId == 0) return 0;
  return g_rustRendererPtr->getLineHeight(fontId);
}
}
