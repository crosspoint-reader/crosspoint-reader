#pragma once
// Host-test GfxRenderer stub for the full-pipeline harness (test/epub_pipeline).
//
// Unlike the minimal gif_decoder stub, this one implements the text-measurement
// API with DETERMINISTIC synthetic metrics so the real layout engine
// (Section/ParsedText/TextBlock/ChapterHtmlSlimParser) can run on the host and
// produce reproducible line breaks and word positions. The numbers do not match
// any real font; equivalence testing only requires that both sides of a
// comparison (two runs, or old-path vs new-path) use the same metrics.
//
// Metric model (all per codepoint, style-aware, kerning-free):
//   advance(cp)        = 6 + (cp % 7)            -> 6..12 px
//   advance(cp, BOLD)  = advance(cp) + 1
//   advance(cp), a full-width CJK character (Han, kana, Hangul, CJK punctuation, full-width forms) = 16 px, one em,
//   as a CJK font sets them
//   space width        = 6 px
//   line height        = 24 px, ascender = 18 px
// Scaled variants multiply and truncate toward zero, mirroring the integer
// snapping the device renderer applies.
#include <Arduino.h>
#include <EpdFontFamily.h>
#include <GlyphProvider.h>
#include <Utf8.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "Epub/CjkPunctuation.h"

enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  // Measuring this codepoint stands in for a TrueType glyph the heap had no room to draw (glyph_miss). 0: none.
  static inline uint32_t starvedCodepoint = 0;
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };

  static constexpr int VIEWABLE_MARGIN_TOP = 9;
  static constexpr int VIEWABLE_MARGIN_RIGHT = 3;
  static constexpr int VIEWABLE_MARGIN_BOTTOM = 3;
  static constexpr int VIEWABLE_MARGIN_LEFT = 3;

  // --- Screen / framebuffer surface (converters, DirectPixelWriter) ---
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  int getDisplayWidth() const { return 480; }
  int getDisplayHeight() const { return 800; }
  uint16_t getDisplayWidthBytes() const { return 60; }
  Orientation getOrientation() const { return Portrait; }
  RenderMode getRenderMode() const { return BW; }
  uint8_t* getWriteTarget() const { return frameBuffer_; }
  int getWriteOriginY() const { return 0; }
  int getWriteRows() const { return 800; }
  void clearScreen(uint8_t = 0xFF) const {}

  // --- Drawing (no-ops: the harness never inspects pixels) ---
  void drawPixel(int, int, bool = true) const {}
  void drawLine(int, int, int, int, bool = true) const {}
  void drawLine(int, int, int, int, int, bool) const {}
  void drawRect(int, int, int, int, bool = true) const {}
  void drawRect(int, int, int, int, int, bool) const {}
  void fillRect(int, int, int, int, bool = true) const {}
  void fillRectDither(int, int, int, int, Color) const {}

  // --- Font lifecycle (built-in-font behavior: everything is always ready) ---
  bool isFontCacheScanning() const { return false; }
  void ensureFontReady(int, const char*) const {}
  void clearFontAccumulation() const {}
  void dropFontMetadata() const {}
  bool restoreFontMetadata() const { return true; }

  // --- Deterministic text metrics ---
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    return getTextAdvanceX(fontId, text, style);
  }
  int getTextWidthScaled(int fontId, const char* text, EpdFontFamily::Style style, float scale) const {
    return static_cast<int>(getTextWidth(fontId, text, style) * scale);
  }
  int getTextAdvanceX(int /*fontId*/, const char* text, EpdFontFamily::Style style) const {
    int w = 0;
    for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p;) {
      uint32_t cp;
      p = decodeUtf8(p, cp);
      if (cp == starvedCodepoint) glyph_miss::noteStarved();
      w += advance(cp, style);
    }
    return w;
  }
  // The stub font has every glyph, CJK included.
  bool hasGlyph(const int /*fontId*/, const uint32_t /*cp*/, const EpdFontFamily::Style /*style*/) const {
    return true;
  }
  // Where a glyph's ink sits in its advance, as a CJK font draws its marks: an opening bracket on the right half,
  // a closing bracket, 。 and 、 on the left half, the other marks centred; anything else fills its advance.
  bool getGlyphInk(const int /*fontId*/, const uint32_t cp, const EpdFontFamily::Style style, int& inkLeft,
                   int& inkWidth, int& adv) const {
    adv = advance(cp, style);
    switch (CjkPunctuation::classOf(cp)) {
      case CjkPunctuation::Class::None:
        inkLeft = 0;
        inkWidth = adv;
        break;
      case CjkPunctuation::Class::Opening:
        inkLeft = (adv + 1) / 2;
        inkWidth = adv - inkLeft;
        break;
      case CjkPunctuation::Class::Closing:
      case CjkPunctuation::Class::FullStop:
      case CjkPunctuation::Class::Comma:
        inkLeft = 0;
        inkWidth = adv / 2;
        break;
      default:
        inkLeft = adv / 4;
        inkWidth = adv / 2;
        break;
    }
    return true;
  }
  int getSpaceWidth(int, EpdFontFamily::Style = EpdFontFamily::REGULAR) const { return 6; }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 6; }
  int getKerning(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 0; }
  int getLineHeight(int) const { return 24; }
  int getLineHeightScaled(int, float scale) const { return static_cast<int>(24 * scale); }
  // Test seam for the image-header heap recovery. 0 (the default) models a host build with
  // no cache manager at all, so callers are told nothing was released. When set, this models
  // the real thing — dropping the SD-font glyph caches frees heap — by reporting success and
  // raising the simulated free heap to that figure.
  uint32_t heapAfterFontCacheRelease = 0;
  bool releaseFontCaches() {
    if (heapAfterFontCacheRelease == 0) return false;
    ESP.setFreeHeap(heapAfterFontCacheRelease);
    return true;
  }
  int getFontAscenderSize(int) const { return 18; }
  int getFontAscenderSizeScaled(int, float scale) const { return static_cast<int>(18 * scale); }
  int getTextHeight(int) const { return 24; }
  // Synthetic ink model: caps span 14 px above the baseline, nothing below
  // (matches the deterministic-metrics philosophy above; 18-14=4 px leading).
  bool getTextInkMetrics(int, const char* text, EpdFontFamily::Style, int* aboveBaseline, int* belowBaseline) const {
    *aboveBaseline = 0;
    *belowBaseline = 0;
    if (text == nullptr || *text == '\0') return false;
    *aboveBaseline = 14;
    return true;
  }

  // --- Text drawing (no-ops) ---
  void drawText(int, int, int, const char*, bool = true, EpdFontFamily::Style = EpdFontFamily::REGULAR) const {}
  void drawTextScaled(int, int, int, const char*, bool, EpdFontFamily::Style, float) const {}
  void drawCenteredText(int, int, const char*, bool = true, EpdFontFamily::Style = EpdFontFamily::REGULAR) const {}
  void drawTextRotated90CW(int, int, int, const char*, bool = true,
                           EpdFontFamily::Style = EpdFontFamily::REGULAR) const {}

 private:
  static int advance(const uint32_t cp, const EpdFontFamily::Style style) {
    const bool fullWidthCjk = utf8IsCjkBreakable(cp) && !(cp >= 0xFF61 && cp <= 0xFFDC);  // halfwidth forms are narrow
    const int base = fullWidthCjk ? 16 : 6 + static_cast<int>(cp % 7);
    // Style is a bitmask; only the BOLD bit affects width in this synthetic model.
    return (style & EpdFontFamily::BOLD) ? base + 1 : base;
  }
  // Minimal UTF-8 decode; invalid bytes consume one byte and yield U+FFFD so
  // malformed input still measures deterministically.
  static const uint8_t* decodeUtf8(const uint8_t* p, uint32_t& cp) {
    if (*p < 0x80) {
      cp = *p;
      return p + 1;
    }
    if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      cp = ((*p & 0x1Fu) << 6) | (p[1] & 0x3Fu);
      return p + 2;
    }
    if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      cp = ((*p & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
      return p + 3;
    }
    if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
      cp = ((*p & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
      return p + 4;
    }
    cp = 0xFFFD;
    return p + 1;
  }

  // Converters write decoded pixels through getWriteTarget(); give them a real
  // buffer so those paths are memory-safe if exercised.
  mutable uint8_t frameBufferStorage_[60 * 800] = {};
  uint8_t* const frameBuffer_ = frameBufferStorage_;
};
