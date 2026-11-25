#include "EpdFontFamily.h"

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  if (useFallbackOnly()) return fallbackFont(style);
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
  const auto* font = getFont(style);
  const auto* fallback = fallbackFont(style);
  font->getTextDimensions(string, w, h, fallback);
  if (font != fallback && useFallbackOnly()) fallback->getTextDimensions(string, w, h, fallback);
}

const EpdFontData* EpdFontFamily::getData(const Style style) const { return getFont(style)->data; }

std::optional<EpdGlyph> EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
  EpdGlyph scratch;
  const auto* glyph = getGlyph(cp, scratch, style, nullptr);
  return glyph ? std::optional<EpdGlyph>(*glyph) : std::nullopt;
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

bool EpdFontFamily::useFallbackOnly() const {
#if CROSSPOINT_VECTOR_FONTS
  return glyphFallback && glyphLoadFailed && glyphLoadFailed->load(std::memory_order_relaxed);
#else
  return false;
#endif
}

const EpdFont* EpdFontFamily::fallbackFont(const Style style) const {
  return glyphFallback ? glyphFallback->getFont(style) : nullptr;
}

const EpdFont* EpdFontFamily::glyphFont(const uint32_t cp, const Style style) const {
  return getFont(style)->fontForCodepoint(cp, fallbackFont(style));
}

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, EpdGlyph& scratch, const Style style,
                                        GlyphBitmapSource* source) const {
  return lookup(cp, scratch, style, source, false);
}

const EpdGlyph* EpdFontFamily::getGlyphMetrics(const uint32_t cp, EpdGlyph& scratch, const Style style,
                                               GlyphBitmapSource* source) const {
  return lookup(cp, scratch, style, source, true);
}

const EpdGlyph* EpdFontFamily::lookup(const uint32_t cp, EpdGlyph& scratch, const Style style,
                                      GlyphBitmapSource* source, bool allowSolid) const {
  const auto read = [&](const EpdFont* font) -> const EpdGlyph* {
    if (source) source->font = font ? font->data : nullptr;
    if (!font) return nullptr;
    auto* index = source ? &source->index : nullptr;
    return allowSolid ? font->getGlyphMetrics(cp, scratch, index) : font->getGlyph(cp, scratch, index);
  };
  const auto* font = glyphFont(cp, style);
  const auto* glyph = read(font);
  return useFallbackOnly() && font != fallbackFont(style) ? read(glyphFont(cp, style)) : glyph;
}
