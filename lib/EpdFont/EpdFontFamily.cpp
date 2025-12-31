#include "EpdFontFamily.h"

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  if (style == BOLD && bold) {
    return bold;
  }
  if (style == ITALIC && italic) {
    return italic;
  }
  if (style == BOLD_ITALIC) {
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
  getFont(style)->getTextDimensions(string, w, h);
}

bool EpdFontFamily::hasPrintableChars(const char* string, const Style style) const {
  return getFont(style)->hasPrintableChars(string);
}

const EpdFontData* EpdFontFamily::getData(const Style style) const { return getFont(style)->data; }

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
  return getFont(style)->getGlyph(cp);
};
