#include "EpdFontFamily.h"

#include <algorithm>

#include "GlyphProvider.h"

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  // Extract font style bits (ignore UNDERLINE bit for font selection)
  const bool hasBold = (style & BOLD) != 0;
  const bool hasItalic = (style & ITALIC) != 0;

  if (hasBold && hasItalic) {
    if (boldItalic) return boldItalic;
    if (bold) return bold;
    if (italic) return italic;
  } else if (hasBold && bold) {
    return bold;
  } else if (hasItalic && italic) {
    return italic;
  }

  return regular;
}

void EpdFontFamily::getTextDimensions(const char* string, int* w, int* h, const Style style) const {
  getFont(style)->getTextDimensions(string, w, h, (style & SMALL_CAPS) != 0);
}

const EpdFontData* EpdFontFamily::getData(const Style style) const { return getFont(style)->data; }

EpdGlyphRef EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const { return getFont(style)->getGlyph(cp); }

int8_t EpdFontFamily::getKerning(const uint32_t leftCp, const uint32_t rightCp, const Style style) const {
  return getFont(style)->getKerning(leftCp, rightCp);
}

void EpdFontFamily::prefetch(const char* utf8Text, const uint8_t styleMask) const {
  const EpdFont* asked[4] = {};
  uint8_t count = 0;
  for (uint8_t style = 0; style < 4; ++style) {
    if (!(styleMask & (1u << style))) continue;
    const EpdFont* font = getFont(static_cast<Style>(style));
    if (!font || !font->data->provider || std::find(asked, asked + count, font) != asked + count) continue;
    asked[count++] = font;
    font->data->provider->prefetch(utf8Text);
  }
}

uint32_t EpdFontFamily::applyLigatures(const uint32_t cp, const char*& text, const Style style) const {
  return getFont(style)->applyLigatures(cp, text);
}
