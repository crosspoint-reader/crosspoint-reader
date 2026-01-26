#include "EpdFontFamily.h"

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  if (style == EpdFontStyles::BOLD && bold) {
    return bold;
  }
  if (style == EpdFontStyles::ITALIC && italic) {
    return italic;
  }
  if (style == EpdFontStyles::BOLD_ITALIC) {
    if (boldItalic) {
      return boldItalic;
    }
    if (bold) {
      return bold;
    }
    if (italic) {
      return italic;
    }
  }

  return regular;
}

void EpdFontFamily::getTextDimensions(const char* string, int* w, int* h, const Style style) const {
  getFont(style)->getTextDimensions(string, w, h, style);
}

int EpdFontFamily::getTextAdvance(const char* string, const Style style) const {
  return getFont(style)->getTextAdvance(string, style);
}

bool EpdFontFamily::hasPrintableChars(const char* string, const Style style) const {
  return getFont(style)->hasPrintableChars(string, style);
}

const EpdFontData* EpdFontFamily::getData(const Style style) const {
  const EpdFont* font = getFont(style);
  return font ? font->getData(style) : nullptr;
}

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
  const EpdFont* font = getFont(style);
  return font ? font->getGlyph(cp, style) : nullptr;
}

const uint8_t* EpdFontFamily::loadGlyphBitmap(const EpdGlyph* glyph, uint8_t* buffer, const Style style) const {
  const EpdFont* font = getFont(style);
  return font ? font->loadGlyphBitmap(glyph, buffer, style) : nullptr;
}
