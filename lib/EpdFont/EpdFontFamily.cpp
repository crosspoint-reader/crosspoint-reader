#include "EpdFontFamily.h"

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  // Extract font style bits; render-time overlay bits do not affect font selection.
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
  getFont(style)->getTextDimensions(string, w, h, fallbackFont(style));
}

const EpdFontData* EpdFontFamily::getData(const Style style) const { return getFont(style)->data; }

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
  const auto* font = glyphFont(cp, style);
  return font ? font->getGlyph(cp) : nullptr;
}

bool EpdFontFamily::hasCodepoint(const uint32_t cp, const Style style) const {
  return getFont(style)->hasCodepoint(cp) || (glyphFallback && fallbackFont(style)->hasCodepoint(cp));
}

int8_t EpdFontFamily::getKerning(const uint32_t leftCp, const uint32_t rightCp, const Style style) const {
  const auto* font = glyphFont(leftCp, style);
  return font && font == glyphFont(rightCp, style) ? font->getKerning(leftCp, rightCp) : 0;
}

uint32_t EpdFontFamily::applyLigatures(const uint32_t cp, const char*& text, const Style style) const {
  const auto* font = getFont(style);
  return !glyphFallback || font->hasCodepoint(cp) ? font->applyLigatures(cp, text) : cp;
}

const EpdFont* EpdFontFamily::fallbackFont(const Style style) const {
  return glyphFallback ? glyphFallback->getFont(style) : nullptr;
}

const EpdFont* EpdFontFamily::glyphFont(const uint32_t cp, const Style style) const {
  return getFont(style)->fontForCodepoint(cp, fallbackFont(style));
}

const EpdGlyph* EpdFontFamily::getGlyphMetrics(const uint32_t cp, EpdGlyph& solidFallback, const Style style,
                                               const EpdFontData** glyphData) const {
  const auto* font = glyphFont(cp, style);
  if (!font) return nullptr;
  if (glyphData) *glyphData = font->data;
  return font->getGlyphMetrics(cp, solidFallback);
}
