// Host-test double for lib/GfxRenderer/GfxRenderer.cpp.
//
// ChapterHtmlSlimParser/TextBlock/Page/ImageBlock only need GfxRenderer for
// font *metrics* (getTextWidth, getTextAdvanceX, getSpaceWidth,
// getFontAscenderSize, getLineHeight) while laying out a chapter; they never
// render() a page in these tests. This double reimplements the measurement
// methods faithfully against the real EpdFontFamily/EpdFont data tables (same
// production code as the real renderer), and no-ops every pixel-drawing
// method (drawText, drawLine, fillRect, ...) since those are never exercised
// here and would otherwise drag in SdCardFont/FontCacheManager/HalGPIO.
//
// This links against the REAL GfxRenderer.h (unchanged) — only the .cpp is a
// test-only replacement, the same pattern test/minibidi_arabic uses for
// Logging.h.

#include "GfxRenderer.h"

#include <Utf8.h>

bool GfxRenderer::isFontCacheScanning() const { return false; }

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) { fontMap.insert({fontId, std::move(font)}); }

void GfxRenderer::freeBwBufferChunks() {}

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style,
                              const BidiUtils::BidiBaseDir /*baseDir*/) const {
  if (text == nullptr || *text == '\0') return 0;
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  int w = 0, h = 0;
  fontIt->second.getTextDimensions(text, &w, &h, style);
  return w;
}

void GfxRenderer::drawText(int /*fontId*/, int /*x*/, int /*y*/, const char* /*text*/, bool /*black*/,
                           EpdFontFamily::Style /*style*/, BidiUtils::BidiBaseDir /*baseDir*/) const {}

void GfxRenderer::drawLine(int /*x1*/, int /*y1*/, int /*x2*/, int /*y2*/, int /*lineWidth*/, bool /*state*/) const {}

void GfxRenderer::fillRect(int /*x*/, int /*y*/, int /*width*/, int /*height*/, bool /*state*/) const {}

void GfxRenderer::ensureSdCardFontReady(int /*fontId*/, const char* /*utf8Text*/, uint8_t /*styleMask*/) const {}

void GfxRenderer::ensureSdCardFontReady(int /*fontId*/, const std::vector<std::string>& /*words*/,
                                        bool /*includeHyphen*/, uint8_t /*styleMask*/) const {}

int GfxRenderer::getFontAscenderSize(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

int GfxRenderer::getLineHeight(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  return fontIt->second.getData(EpdFontFamily::REGULAR)->advanceY;
}

int GfxRenderer::getSpaceWidth(const int fontId, const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const EpdGlyph* spaceGlyph = fontIt->second.getGlyph(' ', style);
  return spaceGlyph ? fp4::toPixel(spaceGlyph->advanceX) : 0;
}

int GfxRenderer::getSpaceAdvance(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                                 const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const auto& font = fontIt->second;
  const EpdGlyph* spaceGlyph = font.getGlyph(' ', style);
  const auto spaceAdvanceFP = spaceGlyph ? static_cast<int32_t>(spaceGlyph->advanceX) : 0;
  const int32_t kernFP = static_cast<int32_t>(font.getKerning(leftCp, ' ', style)) +
                         static_cast<int32_t>(font.getKerning(' ', rightCp, style));
  return fp4::toPixel(spaceAdvanceFP + kernFP);
}

int GfxRenderer::getKerning(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                            const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  return fp4::toPixel(fontIt->second.getKerning(leftCp, rightCp, style));
}

int GfxRenderer::getTextAdvanceX(const int fontId, const char* text, EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;

  uint32_t cp = 0;
  uint32_t prevCp = 0;
  int widthPx = 0;
  int32_t prevAdvanceFP = 0;
  const auto& font = fontIt->second;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) continue;
    cp = font.applyLigatures(cp, text, style);
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);
      widthPx += fp4::toPixel(prevAdvanceFP + kernFP);
    }
    const EpdGlyph* glyph = font.getGlyph(cp, style);
    prevAdvanceFP = glyph ? glyph->advanceX : 0;
    prevCp = cp;
  }
  widthPx += fp4::toPixel(prevAdvanceFP);
  return widthPx;
}

bool GfxRenderer::glyphIntersectsStrip(int /*x0*/, int /*y0*/, int /*x1*/, int /*y1*/) const { return true; }

int GfxRenderer::getScreenWidth() const {
  return (orientation == Portrait || orientation == PortraitInverted) ? panelHeight : panelWidth;
}

int GfxRenderer::getScreenHeight() const {
  return (orientation == Portrait || orientation == PortraitInverted) ? panelWidth : panelHeight;
}
