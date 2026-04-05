#include "GfxRenderer.h"

#include <EpdFontFamily.h>
#include <FontDecompressor.h>
#include <FontManager.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <Utf8.h>

#include "FontCacheManager.h"
#include "VerticalTextUtils.h"

#include <algorithm>

// Built-in CJK UI font (embedded in flash) - 20px only
#include "cjk_ui_font_20.h"

// Reader font IDs (from fontIds.h) - used to determine when to use external
// Chinese font UI fonts should NOT use external font
namespace {
// UI font IDs that should NOT use external reader font
// Values must match src/fontIds.h
constexpr int UI_FONT_IDS[] = {
    -1246724383,  // UI_10_FONT_ID - for status display (battery, page number,
                  // etc.)
    -359249323,   // UI_12_FONT_ID - for status display
    -2089201234,  // UI_20_FONT_ID - primary UI font (menus, titles, settings)
    1073217904    // SMALL_FONT_ID
};
constexpr int UI_FONT_COUNT = sizeof(UI_FONT_IDS) / sizeof(UI_FONT_IDS[0]);

// Reader font IDs - values must match src/fontIds.h
constexpr int READER_FONT_IDS[] = {
    -1905494168,  // BOOKERLY_12_FONT_ID
    1233852315,   // BOOKERLY_14_FONT_ID
    1588566790,   // BOOKERLY_16_FONT_ID
    681638548,    // BOOKERLY_18_FONT_ID
    -1559651934,  // NOTOSANS_12_FONT_ID
    -1014561631,  // NOTOSANS_14_FONT_ID
    -1422711852,  // NOTOSANS_16_FONT_ID
    1237754772,   // NOTOSANS_18_FONT_ID
    1331369208,   // OPENDYSLEXIC_8_FONT_ID
    -1374689004,  // OPENDYSLEXIC_10_FONT_ID
    -795539541,   // OPENDYSLEXIC_12_FONT_ID
    -1676627620   // OPENDYSLEXIC_14_FONT_ID
};
constexpr int READER_FONT_COUNT = sizeof(READER_FONT_IDS) / sizeof(READER_FONT_IDS[0]);

// Check if a Unicode codepoint is CJK (Chinese/Japanese/Korean)
// Only these characters should use the external font width
bool isCjkCodepoint(const uint32_t cp) {
  // CJK Unified Ideographs: U+4E00 - U+9FFF
  if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
  // CJK Unified Ideographs Extension A: U+3400 - U+4DBF
  if (cp >= 0x3400 && cp <= 0x4DBF) return true;
  // CJK Punctuation: U+3000 - U+303F
  if (cp >= 0x3000 && cp <= 0x303F) return true;
  // Hiragana: U+3040 - U+309F
  if (cp >= 0x3040 && cp <= 0x309F) return true;
  // Katakana: U+30A0 - U+30FF
  if (cp >= 0x30A0 && cp <= 0x30FF) return true;
  // CJK Compatibility Ideographs: U+F900 - U+FAFF
  if (cp >= 0xF900 && cp <= 0xFAFF) return true;
  // Fullwidth forms: U+FF00 - U+FFEF
  if (cp >= 0xFF00 && cp <= 0xFFEF) return true;
  // General Punctuation: U+2000 - U+206F (includes smart quotes, ellipsis,
  // dashes)
  if (cp >= 0x2000 && cp <= 0x206F) return true;
  // Number Forms: U+2150 - U+218F (includes Roman numerals)
  if (cp >= 0x2150 && cp <= 0x218F) return true;
  // Enclosed Alphanumerics: U+2460 - U+24FF
  if (cp >= 0x2460 && cp <= 0x24FF) return true;
  // Enclosed CJK Letters and Months: U+3200 - U+32FF
  if (cp >= 0x3200 && cp <= 0x32FF) return true;
  // CJK Compatibility: U+3300 - U+33FF
  if (cp >= 0x3300 && cp <= 0x33FF) return true;
  return false;
}

bool isAsciiDigit(const uint32_t cp) { return cp >= '0' && cp <= '9'; }

bool isAsciiLetter(const uint32_t cp) { return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'); }

int clampExternalAdvance(const int baseWidth, const int spacing) { return std::max(1, baseWidth + spacing); }

bool hasUiGlyphForText(const char* text) {
  if (text == nullptr || *text == '\0') {
    return false;
  }

  const char* ptr = text;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&ptr)))) {
    if (CjkUiFont20::hasCjkUiGlyph(cp)) {
      return true;
    }
  }
  return false;
}

// Check if fontId is a UI font (UI_10, UI_12, UI_20, SMALL_FONT)
// UI fonts may not be registered in fontMap if they use built-in CJK font only
bool isUiFont(int fontId) {
  for (int i = 0; i < UI_FONT_COUNT; i++) {
    if (UI_FONT_IDS[i] == fontId) {
      return true;
    }
  }
  return false;
}
// Check if the font has at least one renderable glyph for any character in text
bool fontHasPrintableChars(const EpdFontFamily& font, const char* text, EpdFontFamily::Style style) {
  const char* ptr = text;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&ptr)))) {
    if (font.getGlyph(cp, style) != nullptr) {
      return true;
    }
  }
  return false;
}
}  // namespace

const uint8_t* GfxRenderer::getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const {
  if (fontData->groups != nullptr) {
    auto* fd = fontCacheManager_ ? fontCacheManager_->getDecompressor() : nullptr;
    if (!fd) {
      LOG_ERR("GFX", "Compressed font but no FontDecompressor set");
      return nullptr;
    }
    uint32_t glyphIndex = static_cast<uint32_t>(glyph - fontData->glyph);
    // For page-buffer hits the pointer is stable for the page lifetime.
    // For hot-group hits it is valid only until the next getBitmap() call — callers
    // must consume it (draw the glyph) before requesting another bitmap.
    return fd->getBitmap(fontData, glyph, glyphIndex);
  }
  // For SD card fonts, check if the glyph was loaded on demand into the overflow
  // buffer.  getOverflowBitmap() returns:
  //   - bitmap pointer for overflow glyphs with bitmap data
  //   - nullptr for overflow glyphs without bitmap data (e.g. space: width=0, height=0)
  //   - nullptr for non-overflow glyphs (normal prewarmed path)
  // We distinguish overflow-with-no-bitmap from non-overflow by checking isOverflowGlyph().
  if (fontData->glyphMissCtx) {
    auto* sdFont = SdCardFont::fromMissCtx(fontData->glyphMissCtx);
    if (sdFont->isOverflowGlyph(glyph)) {
      return sdFont->getOverflowBitmap(glyph);  // may be nullptr for zero-width glyphs
    }
  }
  return &fontData->bitmap[glyph->dataOffset];
}

void GfxRenderer::ensureSdCardFontReady(int fontId, const char* utf8Text) const {
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    // Build a compact advance-only table for layout measurement.
    // Unlike prewarm(), this has no codepoint limit — handles CJK paragraphs
    // with 2000+ unique codepoints without overflow thrashing.
    // Uses 6 bytes per codepoint (vs 16 for full EpdGlyph), no bitmap data.
    int missed = it->second->buildAdvanceTable(utf8Text, 0x0F);
    if (missed > 0) {
      LOG_DBG("GFX", "ensureSdCardFontReady: %d glyph(s) not found", missed);
    }
  }
}

void GfxRenderer::begin() {
  frameBuffer = display.getFrameBuffer();
  if (!frameBuffer) {
    LOG_ERR("GFX", "!! No framebuffer");
    assert(false);
  }
  panelWidth = display.getDisplayWidth();
  panelHeight = display.getDisplayHeight();
  panelWidthBytes = display.getDisplayWidthBytes();
  frameBufferSize = display.getBufferSize();
  bwBufferChunks.assign((frameBufferSize + BW_BUFFER_CHUNK_SIZE - 1) / BW_BUFFER_CHUNK_SIZE, nullptr);
}

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) {
  auto result = fontMap.insert({fontId, font});
  if (!result.second) {
    LOG_ERR("GFX", "Font ID %d already registered, ignoring duplicate", fontId);
  }
}

// Translate logical (x,y) coordinates to physical panel coordinates based on current orientation
// This should always be inlined for better performance
static inline void rotateCoordinates(const GfxRenderer::Orientation orientation, const int x, const int y, int* phyX,
                                     int* phyY, const uint16_t panelWidth, const uint16_t panelHeight) {
  switch (orientation) {
    case GfxRenderer::Portrait: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees clockwise
      *phyX = y;
      *phyY = panelHeight - 1 - x;
      break;
    }
    case GfxRenderer::LandscapeClockwise: {
      // Logical landscape (800x480) rotated 180 degrees (swap top/bottom and left/right)
      *phyX = panelWidth - 1 - x;
      *phyY = panelHeight - 1 - y;
      break;
    }
    case GfxRenderer::PortraitInverted: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees counter-clockwise
      *phyX = panelWidth - 1 - y;
      *phyY = x;
      break;
    }
    case GfxRenderer::LandscapeCounterClockwise: {
      // Logical landscape (800x480) aligned with panel orientation
      *phyX = x;
      *phyY = y;
      break;
    }
  }
}

enum class TextRotation { None, Rotated90CW };

// Shared glyph rendering logic for normal and rotated text.
// Coordinate mapping and cursor advance direction are selected at compile time via the template parameter.
template <TextRotation rotation>
static void renderCharImpl(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                           const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                           const bool pixelState, const EpdFontFamily::Style style) {
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) {
    LOG_ERR("GFX", "No glyph for codepoint %d", cp);
    return;
  }

  const EpdFontData* fontData = fontFamily.getData(style);
  const bool is2Bit = fontData->is2Bit;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int left = glyph->left;
  const int top = glyph->top;

  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);

  if (bitmap != nullptr) {
    // For Normal:  outer loop advances screenY, inner loop advances screenX
    // For Rotated: outer loop advances screenX, inner loop advances screenY (in reverse)
    int outerBase, innerBase;
    if constexpr (rotation == TextRotation::Rotated90CW) {
      outerBase = cursorX + fontData->ascender - top;  // screenX = outerBase + glyphY
      innerBase = cursorY - left;                      // screenY = innerBase - glyphX
    } else {
      outerBase = cursorY - top;   // screenY = outerBase + glyphY
      innerBase = cursorX + left;  // screenX = innerBase + glyphX
    }

    if (is2Bit) {
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++, pixelPosition++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t byte = bitmap[pixelPosition >> 2];
          const uint8_t bit_index = (3 - (pixelPosition & 3)) * 2;
          // the direct bit from the font is 0 -> white, 1 -> light gray, 2 -> dark gray, 3 -> black
          // we swap this to better match the way images and screen think about colors:
          // 0 -> black, 1 -> dark grey, 2 -> light grey, 3 -> white
          const uint8_t bmpVal = 3 - ((byte >> bit_index) & 0x3);

          if (renderMode == GfxRenderer::BW && bmpVal < 3) {
            // Black (also paints over the grays in BW mode)
            renderer.drawPixel(screenX, screenY, pixelState);
          } else if (renderMode == GfxRenderer::GRAYSCALE_MSB && (bmpVal == 1 || (gpio.deviceIsX4() && bmpVal == 2))) {
            // Light gray (also mark the MSB if it's going to be a dark gray too)
            // X3 AA tuning: keep only the darker antialias level to avoid washed text
            // We have to flag pixels in reverse for the gray buffers, as 0 leave alone, 1 update
            renderer.drawPixel(screenX, screenY, false);
          } else if (renderMode == GfxRenderer::GRAYSCALE_LSB && bmpVal == 1) {
            // Dark gray
            renderer.drawPixel(screenX, screenY, false);
          }
        }
      }
    } else {
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++, pixelPosition++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t byte = bitmap[pixelPosition >> 3];
          const uint8_t bit_index = 7 - (pixelPosition & 7);

          if ((byte >> bit_index) & 1) {
            renderer.drawPixel(screenX, screenY, pixelState);
          }
        }
      }
    }
  }
}

// IMPORTANT: This function is in critical rendering path and is called for every pixel. Please keep it as simple and
// efficient as possible.
void GfxRenderer::drawPixel(const int x, const int y, const bool state) const {
  int phyX = 0;
  int phyY = 0;

  // Note: this call should be inlined for better performance
  rotateCoordinates(orientation, x, y, &phyX, &phyY, panelWidth, panelHeight);

  // Bounds checking against runtime panel dimensions
  if (phyX < 0 || phyX >= panelWidth || phyY < 0 || phyY >= panelHeight) {
    // Limit log output to avoid performance issues (log only first few per session)
    static int outsideRangeCount = 0;
    if (outsideRangeCount < 5) {
      LOG_ERR("GFX", "!! Outside range (%d, %d) -> (%d, %d)", x, y, phyX, phyY);
      outsideRangeCount++;
      if (outsideRangeCount == 5) {
        LOG_ERR("GFX", "!! Suppressing further outside range warnings");
      }
    }
    return;
  }

  // Calculate byte position and bit position
  const uint32_t byteIndex = static_cast<uint32_t>(phyY) * panelWidthBytes + (phyX / 8);
  const uint8_t bitPosition = 7 - (phyX % 8);  // MSB first

  // In dark mode, invert the pixel state (black <-> white)
  // But NOT in grayscale mode - grayscale rendering uses special pixel marking
  // And NOT when skipDarkModeForImages is set - cover art should keep original
  // colors
  const bool shouldInvert = darkMode && !skipDarkModeForImages && renderMode == BW;
  const bool actualState = shouldInvert ? !state : state;

  if (actualState) {
    frameBuffer[byteIndex] &= ~(1 << bitPosition);  // Clear bit = black pixel
  } else {
    frameBuffer[byteIndex] |= 1 << bitPosition;  // Set bit = white pixel
  }
}

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  const int effectiveFontId = getEffectiveFontId(fontId);
  if (fontMap.count(effectiveFontId) == 0) {
    // UI fonts may not be in fontMap (they use built-in CJK font)
    if (isUiFont(fontId)) {
      if (text == nullptr || *text == '\0') {
        return 0;
      }

      // Check if external UI font is enabled first
      FontManager& fm = FontManager::getInstance();
      ExternalFont* uiExtFont = nullptr;
      bool hasExternalUiFont = false;

      if (fm.isUiFontEnabled()) {
        uiExtFont = fm.getActiveUiFont();
        if (uiExtFont && uiExtFont->isLoaded()) {
          hasExternalUiFont = true;
        }
      }

      int width = 0;
      const char* ptr = text;
      uint32_t cp;
      while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&ptr)))) {
        bool hasChar = false;

        // First check built-in CJK UI font (Flash access is fast)
        if (CjkUiFont20::hasCjkUiGlyph(cp)) {
          width += CjkUiFont20::getCjkUiGlyphWidth(cp);
          hasChar = true;
        }

        // Fall back to external UI font if enabled (SD card, slow)
        if (!hasChar && hasExternalUiFont) {
          if (uiExtFont->getGlyph(cp) != nullptr) {
            width += uiExtFont->getCharWidth();
            hasChar = true;
          }
        }

        // Default width for unknown characters
        if (!hasChar) {
          width += 10;
        }
      }
      return width;
    }
    LOG_ERR("GFX", "Font %d not found", effectiveFontId);
    return 0;
  }

  FontManager& fm = FontManager::getInstance();

  // Check if using external font for reader fonts
  if (isReaderFont(fontId)) {
    if (fm.isExternalFontEnabled()) {
      ExternalFont* extFont = fm.getActiveFont();
      if (extFont) {
        int width = 0;
        const char* ptr = text;
        const EpdFontFamily& fontFamily = fontMap.at(effectiveFontId);
        const int cjkAdvance = clampExternalAdvance(extFont->getCharWidth(), cjkSpacing);
        uint32_t cp;
        while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&ptr)))) {
          // Fast path: CJK characters always use charWidth - skip SD card read entirely
          if (isCjkCodepoint(cp)) {
            width += cjkAdvance;
            continue;
          }
          const uint8_t* bitmap = extFont->getGlyph(cp);
          if (bitmap) {
            uint8_t advanceX = extFont->getCharWidth();
            extFont->getGlyphMetrics(cp, nullptr, &advanceX);
            int spacing = 0;
            if (isAsciiDigit(cp)) {
              spacing = asciiDigitSpacing;
            } else if (isAsciiLetter(cp)) {
              spacing = asciiLetterSpacing;
            }
            width += clampExternalAdvance(advanceX, spacing);
          } else {
            // Fall back to built-in reader font width
            const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
            if (glyph) {
              width += glyph->advanceX;
            } else {
              width += 10;
            }
          }
        }
        return width;
      }
    }
  } else {
    // UI font - calculate width with built-in UI font (includes CJK and
    // English) Check if text contains any characters in our UI font or
    // CJK characters that may use external font fallback
    const char* checkPtr = text;
    bool hasUiFontChar = false;
    bool hasCjkChar = false;
    uint32_t testCp;
    while ((testCp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&checkPtr)))) {
      if (CjkUiFont20::hasCjkUiGlyph(testCp)) {
        hasUiFontChar = true;
      }
      if (isCjkCodepoint(testCp)) {
        hasCjkChar = true;
      }
    }

    // Process if text contains UI font chars or CJK chars (for external font
    // fallback)
    if (hasUiFontChar || hasCjkChar) {
      int width = 0;
      const char* ptr = text;
      const auto fontIt = fontMap.find(effectiveFontId);
      if (fontIt == fontMap.end()) {
        LOG_ERR("GFX", "Font %d not found in UI CJK path", effectiveFontId);
        return 0;
      }
      const EpdFontFamily& fontFamily = fontIt->second;
      FontManager& fmLocal = FontManager::getInstance();
      uint32_t cp;
      while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&ptr)))) {
        // Check if character is in our UI font (includes CJK and English)
        uint8_t actualWidth = CjkUiFont20::getCjkUiGlyphWidth(cp);

        if (actualWidth > 0) {
          // Character is in UI font: use actual proportional width
          width += actualWidth;
        } else if (isCjkCodepoint(cp)) {
          // CJK character not in UI font: try UI external font, then reader
          ExternalFont* uiExtFont = nullptr;
          if (fmLocal.isUiFontEnabled()) {
            uiExtFont = fmLocal.getActiveUiFont();
          } else if (fmLocal.isExternalFontEnabled()) {
            uiExtFont = fmLocal.getActiveFont();
          }

          if (uiExtFont) {
            uint8_t minX, advanceX;
            if (uiExtFont->getGlyphMetrics(cp, &minX, &advanceX)) {
              width += advanceX;
            } else {
              // External font doesn't have this glyph either, use its charWidth
              width += uiExtFont->getCharWidth();
            }
          } else {
            // No external font, use built-in font width
            const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
            if (glyph) {
              width += glyph->advanceX;
            }
          }
        } else {
          // Character not in UI font: use built-in font width
          const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
          if (glyph) {
            width += glyph->advanceX;
          }
        }
      }
      return width;
    }
  }

  int w = 0, h = 0;
  fontMap.at(effectiveFontId).getTextDimensions(text, &w, &h, style);
  return w;
}

void GfxRenderer::drawCenteredText(const int fontId, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style) const {
  const int x = (getScreenWidth() - getTextWidth(fontId, text, style)) / 2;
  drawText(fontId, x, y, text, black, style);
}

void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style) const {
  const int yPos = y + getFontAscenderSize(fontId);
  int xpos = x;

  // cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    fontCacheManager_->recordText(text, fontId, style);
    return;
  }

  // For external reader font IDs (negative, but NOT UI fonts), use a default
  // built-in font as fallback
  const int effectiveFontId = getEffectiveFontId(fontId);

  if (fontMap.count(effectiveFontId) == 0) {
    // UI fonts may not be in fontMap (they use built-in CJK font)
    if (isUiFont(fontId)) {
      // Check if external UI font is enabled (only used as fallback)
      FontManager& fm = FontManager::getInstance();
      bool hasExternalUiFont = false;
      ExternalFont* uiExtFont = nullptr;

      if (fm.isUiFontEnabled()) {
        uiExtFont = fm.getActiveUiFont();
        if (uiExtFont && uiExtFont->isLoaded()) {
          hasExternalUiFont = true;
        }
      }

      // Use the yPos already calculated at the start of drawText (includes
      // ascender offset)
      int xPos = x;
      const char* ptr = text;
      uint32_t cp;

      while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&ptr)))) {
        bool rendered = false;

        // First check built-in CJK UI font (Flash access is fast)
        if (CjkUiFont20::hasCjkUiGlyph(cp)) {
          const uint8_t* bitmap = CjkUiFont20::getCjkUiGlyph(cp);
          uint8_t advanceWidth = CjkUiFont20::getCjkUiGlyphWidth(cp);
          const uint8_t height = CjkUiFont20::CJK_UI_FONT_HEIGHT;
          const uint8_t bytesPerRow = CjkUiFont20::CJK_UI_FONT_BYTES_PER_ROW;
          const uint8_t glyphWidth = CjkUiFont20::CJK_UI_FONT_WIDTH;

          // Reduce spacing for CJK characters
          if (advanceWidth >= 20) {
            advanceWidth = 18;
          }

          for (uint8_t glyphY = 0; glyphY < height; glyphY++) {
            for (uint8_t glyphX = 0; glyphX < glyphWidth; glyphX++) {
              const uint8_t byteIndex = glyphY * bytesPerRow + (glyphX / 8);
              const uint8_t bitIndex = 7 - (glyphX % 8);
              const uint8_t byte = pgm_read_byte(&bitmap[byteIndex]);
              if ((byte >> bitIndex) & 1) {
                drawPixel(xPos + glyphX, y + glyphY, black);
              }
            }
          }
          xPos += advanceWidth;
          rendered = true;
        }

        // Fall back to external UI font if built-in doesn't have this glyph
        if (!rendered && hasExternalUiFont) {
          const uint8_t* bitmap = uiExtFont->getGlyph(cp);
          if (bitmap) {
            const uint8_t charWidth = uiExtFont->getCharWidth();
            const uint8_t charHeight = uiExtFont->getCharHeight();
            const uint8_t bytesPerRow = (charWidth + 7) / 8;

            for (uint8_t glyphY = 0; glyphY < charHeight; glyphY++) {
              for (uint8_t glyphX = 0; glyphX < charWidth; glyphX++) {
                const uint8_t byteIndex = glyphY * bytesPerRow + (glyphX / 8);
                const uint8_t bitIndex = 7 - (glyphX % 8);
                const uint8_t byte = bitmap[byteIndex];
                if ((byte >> bitIndex) & 1) {
                  drawPixel(xPos + glyphX, y + glyphY, black);
                }
              }
            }
            uint8_t advanceX = charWidth;
            uiExtFont->getGlyphMetrics(cp, nullptr, &advanceX);
            xPos += advanceX;
            rendered = true;
          }
        }

        // Default advance for unknown characters
        if (!rendered) {
          xPos += 10;
        }
      }
      return;
    }
    LOG_ERR("GFX", "Font %d not found", effectiveFontId);
    return;
  }
  const auto& font = fontMap.at(effectiveFontId);

  // no printable characters
  if (!fontHasPrintableChars(font, text, style)) {
    FontManager& fm = FontManager::getInstance();
    if (isReaderFont(fontId)) {
      if (!fm.isExternalFontEnabled()) {
        return;
      }
    } else {
      const bool hasUiGlyph = hasUiGlyphForText(text);
      if (!hasUiGlyph && !fm.isUiFontEnabled() && !fm.isExternalFontEnabled()) {
        return;
      }
    }
  }

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    renderChar(effectiveFontId, font, cp, &xpos, &yPos, black, style);
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const bool state) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  if (x1 == x2) {
    if (y2 < y1) {
      std::swap(y1, y2);
    }
    for (int y = y1; y <= y2; y++) {
      drawPixel(x1, y, state);
    }
  } else if (y1 == y2) {
    if (x2 < x1) {
      std::swap(x1, x2);
    }
    for (int x = x1; x <= x2; x++) {
      drawPixel(x, y1, state);
    }
  } else {
    // Bresenham's line algorithm — integer arithmetic only
    int dx = x2 - x1;
    int dy = y2 - y1;
    int sx = (dx > 0) ? 1 : -1;
    int sy = (dy > 0) ? 1 : -1;
    dx = sx * dx;  // abs
    dy = sy * dy;  // abs

    int err = dx - dy;
    while (true) {
      drawPixel(x1, y1, state);
      if (x1 == x2 && y1 == y2) break;
      int e2 = 2 * err;
      if (e2 > -dy) {
        err -= dy;
        x1 += sx;
      }
      if (e2 < dx) {
        err += dx;
        y1 += sy;
      }
    }
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const int lineWidth, const bool state) const {
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x1, y1 + i, x2, y2 + i, state);
  }
}

void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const bool state) const {
  drawLine(x, y, x + width - 1, y, state);
  drawLine(x + width - 1, y, x + width - 1, y + height - 1, state);
  drawLine(x + width - 1, y + height - 1, x, y + height - 1, state);
  drawLine(x, y, x, y + height - 1, state);
}

// Border is inside the rectangle
void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const int lineWidth,
                           const bool state) const {
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x + i, y + i, x + width - i, y + i, state);
    drawLine(x + width - i, y + i, x + width - i, y + height - i, state);
    drawLine(x + width - i, y + height - i, x + i, y + height - i, state);
    drawLine(x + i, y + height - i, x + i, y + i, state);
  }
}

void GfxRenderer::drawArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir,
                          const int lineWidth, const bool state) const {
  const int stroke = std::min(lineWidth, maxRadius);
  const int innerRadius = std::max(maxRadius - stroke, 0);
  const int outerRadiusSq = maxRadius * maxRadius;
  const int innerRadiusSq = innerRadius * innerRadius;
  for (int dy = 0; dy <= maxRadius; ++dy) {
    for (int dx = 0; dx <= maxRadius; ++dx) {
      const int distSq = dx * dx + dy * dy;
      if (distSq > outerRadiusSq || distSq < innerRadiusSq) {
        continue;
      }
      const int px = cx + xDir * dx;
      const int py = cy + yDir * dy;
      drawPixel(px, py, state);
    }
  }
};

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool state) const {
  drawRoundedRect(x, y, width, height, lineWidth, cornerRadius, true, true, true, true, state);
}

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool roundTopLeft, bool roundTopRight, bool roundBottomLeft,
                                  bool roundBottomRight, bool state) const {
  if (lineWidth <= 0 || width <= 0 || height <= 0) {
    return;
  }

  const int maxRadius = std::min({cornerRadius, width / 2, height / 2});
  if (maxRadius <= 0) {
    drawRect(x, y, width, height, lineWidth, state);
    return;
  }

  const int stroke = std::min(lineWidth, maxRadius);
  const int right = x + width - 1;
  const int bottom = y + height - 1;

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    if (roundTopLeft || roundTopRight) {
      fillRect(x + maxRadius, y, horizontalWidth, stroke, state);
    }
    if (roundBottomLeft || roundBottomRight) {
      fillRect(x + maxRadius, bottom - stroke + 1, horizontalWidth, stroke, state);
    }
  }

  const int verticalHeight = height - 2 * maxRadius;
  if (verticalHeight > 0) {
    if (roundTopLeft || roundBottomLeft) {
      fillRect(x, y + maxRadius, stroke, verticalHeight, state);
    }
    if (roundTopRight || roundBottomRight) {
      fillRect(right - stroke + 1, y + maxRadius, stroke, verticalHeight, state);
    }
  }

  if (roundTopLeft) {
    drawArc(maxRadius, x + maxRadius, y + maxRadius, -1, -1, lineWidth, state);
  }
  if (roundTopRight) {
    drawArc(maxRadius, right - maxRadius, y + maxRadius, 1, -1, lineWidth, state);
  }
  if (roundBottomRight) {
    drawArc(maxRadius, right - maxRadius, bottom - maxRadius, 1, 1, lineWidth, state);
  }
  if (roundBottomLeft) {
    drawArc(maxRadius, x + maxRadius, bottom - maxRadius, -1, 1, lineWidth, state);
  }
}

void GfxRenderer::fillRect(const int x, const int y, const int width, const int height, const bool state) const {
  for (int fillY = y; fillY < y + height; fillY++) {
    drawLine(x, fillY, x + width - 1, fillY, state);
  }
}

// NOTE: Those are in critical path, and need to be templated to avoid runtime checks for every pixel.
// Any branching must be done outside the loops to avoid performance degradation.
template <>
void GfxRenderer::drawPixelDither<Color::Clear>(const int x, const int y) const {
  // Do nothing
}

template <>
void GfxRenderer::drawPixelDither<Color::Black>(const int x, const int y) const {
  drawPixel(x, y, true);
}

template <>
void GfxRenderer::drawPixelDither<Color::White>(const int x, const int y) const {
  drawPixel(x, y, false);
}

template <>
void GfxRenderer::drawPixelDither<Color::LightGray>(const int x, const int y) const {
  drawPixel(x, y, x % 2 == 0 && y % 2 == 0);
}

template <>
void GfxRenderer::drawPixelDither<Color::DarkGray>(const int x, const int y) const {
  drawPixel(x, y, (x + y) % 2 == 0);  // TODO: maybe find a better pattern?
}

void GfxRenderer::fillRectDither(const int x, const int y, const int width, const int height, Color color) const {
  if (color == Color::Clear) {
  } else if (color == Color::Black) {
    fillRect(x, y, width, height, true);
  } else if (color == Color::White) {
    fillRect(x, y, width, height, false);
  } else if (color == Color::LightGray) {
    for (int fillY = y; fillY < y + height; fillY++) {
      for (int fillX = x; fillX < x + width; fillX++) {
        drawPixelDither<Color::LightGray>(fillX, fillY);
      }
    }
  } else if (color == Color::DarkGray) {
    for (int fillY = y; fillY < y + height; fillY++) {
      for (int fillX = x; fillX < x + width; fillX++) {
        drawPixelDither<Color::DarkGray>(fillX, fillY);
      }
    }
  }
}

template <Color color>
void GfxRenderer::fillArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir) const {
  const int radiusSq = maxRadius * maxRadius;
  for (int dy = 0; dy <= maxRadius; ++dy) {
    for (int dx = 0; dx <= maxRadius; ++dx) {
      const int distSq = dx * dx + dy * dy;
      const int px = cx + xDir * dx;
      const int py = cy + yDir * dy;
      if (distSq <= radiusSq) {
        drawPixelDither<color>(px, py);
      }
    }
  }
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  const Color color) const {
  fillRoundedRect(x, y, width, height, cornerRadius, true, true, true, true, color);
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  bool roundTopLeft, bool roundTopRight, bool roundBottomLeft, bool roundBottomRight,
                                  const Color color) const {
  if (width <= 0 || height <= 0) {
    return;
  }

  // Assume if we're not rounding all corners then we are only rounding one side
  const int roundedSides = (!roundTopLeft || !roundTopRight || !roundBottomLeft || !roundBottomRight) ? 1 : 2;
  const int maxRadius = std::min({cornerRadius, width / roundedSides, height / roundedSides});
  if (maxRadius <= 0) {
    fillRectDither(x, y, width, height, color);
    return;
  }

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    fillRectDither(x + maxRadius + 1, y, horizontalWidth - 2, height, color);
  }

  const int leftFillTop = y + (roundTopLeft ? (maxRadius + 1) : 0);
  const int leftFillBottom = y + height - 1 - (roundBottomLeft ? (maxRadius + 1) : 0);
  if (leftFillBottom >= leftFillTop) {
    fillRectDither(x, leftFillTop, maxRadius + 1, leftFillBottom - leftFillTop + 1, color);
  }

  const int rightFillTop = y + (roundTopRight ? (maxRadius + 1) : 0);
  const int rightFillBottom = y + height - 1 - (roundBottomRight ? (maxRadius + 1) : 0);
  if (rightFillBottom >= rightFillTop) {
    fillRectDither(x + width - maxRadius - 1, rightFillTop, maxRadius + 1, rightFillBottom - rightFillTop + 1, color);
  }

  auto fillArcTemplated = [this](int maxRadius, int cx, int cy, int xDir, int yDir, Color color) {
    switch (color) {
      case Color::Clear:
        break;
      case Color::Black:
        fillArc<Color::Black>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::White:
        fillArc<Color::White>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::LightGray:
        fillArc<Color::LightGray>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::DarkGray:
        fillArc<Color::DarkGray>(maxRadius, cx, cy, xDir, yDir);
        break;
    }
  };

  if (roundTopLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + maxRadius, -1, -1, color);
  }

  if (roundTopRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + maxRadius, 1, -1, color);
  }

  if (roundBottomRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + height - maxRadius - 1, 1, 1, color);
  }

  if (roundBottomLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + height - maxRadius - 1, -1, 1, color);
  }
}

void GfxRenderer::drawImage(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(orientation, x, y, &rotatedX, &rotatedY, panelWidth, panelHeight);
  // Rotate origin corner
  switch (orientation) {
    case Portrait:
      rotatedY = rotatedY - height;
      break;
    case PortraitInverted:
      rotatedX = rotatedX - width;
      break;
    case LandscapeClockwise:
      rotatedY = rotatedY - height;
      rotatedX = rotatedX - width;
      break;
    case LandscapeCounterClockwise:
      break;
  }
  // TODO: Rotate bits
  display.drawImage(bitmap, rotatedX, rotatedY, width, height);

  // In dark mode, invert the drawn region so icons adapt to dark background.
  // drawImage bypasses drawPixel (raw memcpy), so we invert the bytes post-hoc.
  if (darkMode && renderMode == BW) {
    const int widthBytes = width / 8;
    for (int row = 0; row < height; row++) {
      const int bufY = rotatedY + row;
      if (bufY < 0 || bufY >= HalDisplay::DISPLAY_HEIGHT) continue;
      const int bufStart = bufY * HalDisplay::DISPLAY_WIDTH_BYTES + (rotatedX / 8);
      for (int b = 0; b < widthBytes; b++) {
        const int idx = bufStart + b;
        if (idx >= 0 && idx < HalDisplay::BUFFER_SIZE) {
          frameBuffer[idx] = ~frameBuffer[idx];
        }
      }
    }
  }
}

void GfxRenderer::drawIcon(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  // Icon bitmaps are authored to match the historical drawImageTransparent
  // path used by UI themes (portrait physical placement with transposed axes).
  // Recreate that logical mapping, then render through drawPixel() so current
  // renderer orientation (including PortraitInverted/Landscape) is applied.
  // For icon masks: 0 = black pixel, 1 = transparent.
  const int rowBytes = (width + 7) / 8;
  for (int row = 0; row < height; row++) {
    for (int col = 0; col < width; col++) {
      const int byteIndex = row * rowBytes + (col / 8);
      const int bitIndex = 7 - (col % 8);
      const bool transparent = ((bitmap[byteIndex] >> bitIndex) & 0x1) != 0;
      if (!transparent) {
        const int legacyX = x + width - 1 - row;
        const int legacyY = y + col;
        drawPixel(legacyX, legacyY, true);
      }
    }
  }
}

void GfxRenderer::drawBitmap(const Bitmap& bitmap, const int x, const int y, const int maxWidth, const int maxHeight,
                             const float cropX, const float cropY) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  // Cover art and images should keep their original colors in dark mode
  skipDarkModeForImages = true;
  auto cleanup = [this]() { skipDarkModeForImages = false; };

  // For 1-bit bitmaps, use optimized 1-bit rendering path (no crop support for 1-bit)
  if (bitmap.is1Bit() && cropX == 0.0f && cropY == 0.0f) {
    drawBitmap1Bit(bitmap, x, y, maxWidth, maxHeight);
    cleanup();
    return;
  }

  float scale = 1.0f;
  bool isScaled = false;
  int cropPixX = std::floor(bitmap.getWidth() * cropX / 2.0f);
  int cropPixY = std::floor(bitmap.getHeight() * cropY / 2.0f);
  LOG_DBG("GFX", "Cropping %dx%d by %dx%d pix, is %s", bitmap.getWidth(), bitmap.getHeight(), cropPixX, cropPixY,
          bitmap.isTopDown() ? "top-down" : "bottom-up");

  const float croppedWidth = (1.0f - cropX) * static_cast<float>(bitmap.getWidth());
  const float croppedHeight = (1.0f - cropY) * static_cast<float>(bitmap.getHeight());
  bool hasTargetBounds = false;
  float fitScale = 1.0f;

  if (maxWidth > 0 && croppedWidth > 0.0f) {
    fitScale = static_cast<float>(maxWidth) / croppedWidth;
    hasTargetBounds = true;
  }

  if (maxHeight > 0 && croppedHeight > 0.0f) {
    const float heightScale = static_cast<float>(maxHeight) / croppedHeight;
    fitScale = hasTargetBounds ? std::min(fitScale, heightScale) : heightScale;
    hasTargetBounds = true;
  }

  if (hasTargetBounds && fitScale < 1.0f) {
    scale = fitScale;
    isScaled = true;
  }
  LOG_DBG("GFX", "Scaling by %f - %s", scale, isScaled ? "scaled" : "not scaled");

  if (darkMode && renderMode == BW) {
    const int sourceWidth = static_cast<int>((1.0f - cropX) * bitmap.getWidth() - cropPixX);
    const int sourceHeight = static_cast<int>((1.0f - cropY) * bitmap.getHeight() - cropPixY);
    const int scaledWidth = isScaled ? static_cast<int>(std::floor(sourceWidth * scale)) : sourceWidth;
    const int scaledHeight = isScaled ? static_cast<int>(std::floor(sourceHeight * scale)) : sourceHeight;
    fillRect(x, y, scaledWidth, scaledHeight, false);
  }

  // Calculate output row size (2 bits per pixel, packed into bytes)
  // IMPORTANT: Use int, not uint8_t, to avoid overflow for images > 1020 pixels wide
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto* outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
  auto* rowBytes = static_cast<uint8_t*>(malloc(bitmap.getRowBytes()));

  if (!outputRow || !rowBytes) {
    LOG_ERR("GFX", "!! Failed to allocate BMP row buffers");
    free(outputRow);
    free(rowBytes);
    return;
  }

  for (int bmpY = 0; bmpY < (bitmap.getHeight() - cropPixY); bmpY++) {
    // The BMP's (0, 0) is the bottom-left corner (if the height is positive, top-left if negative).
    // Screen's (0, 0) is the top-left corner.
    int screenY = -cropPixY + (bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY);
    if (isScaled) {
      screenY = std::floor(screenY * scale);
    }
    screenY += y;  // the offset should not be scaled
    if (screenY >= getScreenHeight()) {
      break;
    }

    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from bitmap", bmpY);
      free(outputRow);
      free(rowBytes);
      return;
    }

    if (screenY < 0) {
      continue;
    }

    if (bmpY < cropPixY) {
      // Skip the row if it's outside the crop area
      continue;
    }

    for (int bmpX = cropPixX; bmpX < bitmap.getWidth() - cropPixX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) {
        screenX = std::floor(screenX * scale);
      }
      screenX += x;  // the offset should not be scaled
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      if (renderMode == BW && val < 3) {
        drawPixel(screenX, screenY);
      } else if (renderMode == GRAYSCALE_MSB && (val == 1 || (gpio.deviceIsX4() && val == 2))) {
        drawPixel(screenX, screenY, false);
      } else if (renderMode == GRAYSCALE_LSB && val == 1) {
        drawPixel(screenX, screenY, false);
      }
    }
  }

  free(outputRow);
  free(rowBytes);
  cleanup();
}

void GfxRenderer::drawBitmap1Bit(const Bitmap& bitmap, const int x, const int y, const int maxWidth,
                                 const int maxHeight) const {
  // Cover art and images should keep their original colors in dark mode
  skipDarkModeForImages = true;
  auto cleanup = [this]() { skipDarkModeForImages = false; };

  float scale = 1.0f;
  bool isScaled = false;
  if (maxWidth > 0 && bitmap.getWidth() > maxWidth) {
    scale = static_cast<float>(maxWidth) / static_cast<float>(bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && bitmap.getHeight() > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) / static_cast<float>(bitmap.getHeight()));
    isScaled = true;
  }

  // In dark mode, pre-fill image area with white since white pixels are
  // not explicitly drawn (they rely on clearScreen background).
  if (darkMode && renderMode == BW) {
    const int displayWidth = isScaled ? static_cast<int>(std::floor(bitmap.getWidth() * scale)) : bitmap.getWidth();
    const int displayHeight = isScaled ? static_cast<int>(std::floor(bitmap.getHeight() * scale)) : bitmap.getHeight();
    fillRect(x, y, displayWidth, displayHeight, false);
  }

  // For 1-bit BMP, output is still 2-bit packed (for consistency with readNextRow)
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto* outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
  auto* rowBytes = static_cast<uint8_t*>(malloc(bitmap.getRowBytes()));

  if (!outputRow || !rowBytes) {
    LOG_ERR("GFX", "!! Failed to allocate 1-bit BMP row buffers");
    free(outputRow);
    free(rowBytes);
    return;
  }

  for (int bmpY = 0; bmpY < bitmap.getHeight(); bmpY++) {
    // Read rows sequentially using readNextRow
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from 1-bit bitmap", bmpY);
      free(outputRow);
      free(rowBytes);
      return;
    }

    // Calculate screen Y based on whether BMP is top-down or bottom-up
    const int bmpYOffset = bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY;
    int screenY = y + (isScaled ? static_cast<int>(std::floor(bmpYOffset * scale)) : bmpYOffset);
    if (screenY >= getScreenHeight()) {
      continue;  // Continue reading to keep row counter in sync
    }
    if (screenY < 0) {
      continue;
    }

    for (int bmpX = 0; bmpX < bitmap.getWidth(); bmpX++) {
      int screenX = x + (isScaled ? static_cast<int>(std::floor(bmpX * scale)) : bmpX);
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      // Get 2-bit value (result of readNextRow quantization)
      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      // For 1-bit source: 0 or 1 -> map to black (0,1,2) or white (3)
      // val < 3 means black pixel (draw it)
      if (val < 3) {
        drawPixel(screenX, screenY, true);
      }
      // White pixels (val == 3) are not drawn (leave background)
    }
  }

  free(outputRow);
  free(rowBytes);
  cleanup();
}

void GfxRenderer::fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state) const {
  if (numPoints < 3) return;

  // Find bounding box
  int minY = yPoints[0], maxY = yPoints[0];
  for (int i = 1; i < numPoints; i++) {
    if (yPoints[i] < minY) minY = yPoints[i];
    if (yPoints[i] > maxY) maxY = yPoints[i];
  }

  // Clip to screen
  if (minY < 0) minY = 0;
  if (maxY >= getScreenHeight()) maxY = getScreenHeight() - 1;

  // Allocate node buffer for scanline algorithm
  auto* nodeX = static_cast<int*>(malloc(numPoints * sizeof(int)));
  if (!nodeX) {
    LOG_ERR("GFX", "!! Failed to allocate polygon node buffer");
    return;
  }

  // Scanline fill algorithm
  for (int scanY = minY; scanY <= maxY; scanY++) {
    int nodes = 0;

    // Find all intersection points with edges
    int j = numPoints - 1;
    for (int i = 0; i < numPoints; i++) {
      if ((yPoints[i] < scanY && yPoints[j] >= scanY) || (yPoints[j] < scanY && yPoints[i] >= scanY)) {
        // Calculate X intersection using fixed-point to avoid float
        int dy = yPoints[j] - yPoints[i];
        if (dy != 0) {
          nodeX[nodes++] = xPoints[i] + (scanY - yPoints[i]) * (xPoints[j] - xPoints[i]) / dy;
        }
      }
      j = i;
    }

    // Sort nodes by X (simple bubble sort, numPoints is small)
    for (int i = 0; i < nodes - 1; i++) {
      for (int k = i + 1; k < nodes; k++) {
        if (nodeX[i] > nodeX[k]) {
          int temp = nodeX[i];
          nodeX[i] = nodeX[k];
          nodeX[k] = temp;
        }
      }
    }

    // Fill between pairs of nodes
    for (int i = 0; i < nodes - 1; i += 2) {
      int startX = nodeX[i];
      int endX = nodeX[i + 1];

      // Clip to screen
      if (startX < 0) startX = 0;
      if (endX >= getScreenWidth()) endX = getScreenWidth() - 1;

      // Draw horizontal line
      for (int x = startX; x <= endX; x++) {
        drawPixel(x, scanY, state);
      }
    }
  }

  free(nodeX);
}

// For performance measurement (using static to allow "const" methods)
static unsigned long start_ms = 0;

void GfxRenderer::clearScreen(const uint8_t color) const {
  start_ms = millis();
  // Dark mode: invert the default white (0xFF) background to black (0x00).
  // Grayscale prep passes clearScreen(0x00) and is not affected.
  display.clearScreen((darkMode && color == 0xFF) ? 0x00 : color);
}

void GfxRenderer::invertScreen() const {
  for (uint32_t i = 0; i < frameBufferSize; i++) {
    frameBuffer[i] = ~frameBuffer[i];
  }
}

void GfxRenderer::displayBuffer(const HalDisplay::RefreshMode refreshMode) const {
  auto elapsed = millis() - start_ms;
  LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBuffer", elapsed);
  display.displayBuffer(refreshMode, fadingFix);
}

std::string GfxRenderer::truncatedText(const int fontId, const char* text, const int maxWidth,
                                       const EpdFontFamily::Style style) const {
  if (!text || maxWidth <= 0) return "";

  std::string item = text;
  // U+2026 HORIZONTAL ELLIPSIS (UTF-8: 0xE2 0x80 0xA6)
  const char* ellipsis = "\xe2\x80\xa6";
  int textWidth = getTextWidth(fontId, item.c_str(), style);
  if (textWidth <= maxWidth) {
    // Text fits, return as is
    return item;
  }

  while (!item.empty() && getTextWidth(fontId, (item + ellipsis).c_str(), style) >= maxWidth) {
    utf8RemoveLastChar(item);
  }

  return item.empty() ? ellipsis : item + ellipsis;
}

std::vector<std::string> GfxRenderer::wrappedText(const int fontId, const char* text, const int maxWidth,
                                                  const int maxLines, const EpdFontFamily::Style style) const {
  std::vector<std::string> lines;

  if (!text || maxWidth <= 0 || maxLines <= 0) return lines;

  std::string remaining = text;
  std::string currentLine;

  while (!remaining.empty()) {
    if (static_cast<int>(lines.size()) == maxLines - 1) {
      // Last available line: combine any word already started on this line with
      // the rest of the text, then let truncatedText fit it with an ellipsis.
      std::string lastContent = currentLine.empty() ? remaining : currentLine + " " + remaining;
      lines.push_back(truncatedText(fontId, lastContent.c_str(), maxWidth, style));
      return lines;
    }

    // Find next word
    size_t spacePos = remaining.find(' ');
    std::string word;

    if (spacePos == std::string::npos) {
      word = remaining;
      remaining.clear();
    } else {
      word = remaining.substr(0, spacePos);
      remaining.erase(0, spacePos + 1);
    }

    std::string testLine = currentLine.empty() ? word : currentLine + " " + word;

    if (getTextWidth(fontId, testLine.c_str(), style) <= maxWidth) {
      currentLine = testLine;
    } else {
      if (!currentLine.empty()) {
        lines.push_back(currentLine);
        // If the carried-over word itself exceeds maxWidth, truncate it and
        // push it as a complete line immediately — storing it in currentLine
        // would allow a subsequent short word to be appended after the ellipsis.
        if (getTextWidth(fontId, word.c_str(), style) > maxWidth) {
          lines.push_back(truncatedText(fontId, word.c_str(), maxWidth, style));
          currentLine.clear();
          if (static_cast<int>(lines.size()) >= maxLines) return lines;
        } else {
          currentLine = word;
        }
      } else {
        // Single word wider than maxWidth: truncate and stop to avoid complicated
        // splitting rules (different between languages). Results in an aesthetically
        // pleasing end.
        lines.push_back(truncatedText(fontId, word.c_str(), maxWidth, style));
        return lines;
      }
    }
  }

  if (!currentLine.empty() && static_cast<int>(lines.size()) < maxLines) {
    lines.push_back(currentLine);
  }

  return lines;
}

// Note: Internal driver treats screen in command orientation; this library exposes a logical orientation
int GfxRenderer::getScreenWidth() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 480px wide in portrait logical coordinates
      return panelHeight;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 800px wide in landscape logical coordinates
      return panelWidth;
  }
  return panelHeight;
}

int GfxRenderer::getScreenHeight() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 800px tall in portrait logical coordinates
      return panelWidth;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 480px tall in landscape logical coordinates
      return panelHeight;
  }
  return panelWidth;
}

int GfxRenderer::getSpaceWidth(const int fontId, const EpdFontFamily::Style style) const {
  // Advance table fast-path for SD card fonts during layout
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    return fp4::toPixel(sdIt->second->getAdvance(' ', static_cast<uint8_t>(style)));
  }

  const int effectiveFontId = getEffectiveFontId(fontId);
  if (fontMap.count(effectiveFontId) == 0) {
    // UI fonts may not be in fontMap (they use built-in CJK font)
    if (isUiFont(fontId)) {
      return 10;  // Default space width for 20px UI font
    }
    LOG_ERR("GFX", "Font %d not found", effectiveFontId);
    return 0;
  }

  // Use external font's space advance when active - keeps word/space metrics consistent
  if (isReaderFont(fontId)) {
    FontManager& fm = FontManager::getInstance();
    if (fm.isExternalFontEnabled()) {
      ExternalFont* extFont = fm.getActiveFont();
      if (extFont && extFont->getGlyph(' ')) {
        uint8_t advanceX = extFont->getCharWidth();
        extFont->getGlyphMetrics(' ', nullptr, &advanceX);
        return clampExternalAdvance(advanceX, 0);
      }
    }
  }

  const EpdGlyph* spaceGlyph = fontMap.at(effectiveFontId).getGlyph(' ', style);
  if (!spaceGlyph) return 0;
  int adv = fp4::toPixel(spaceGlyph->advanceX);
  const uint16_t scale = getSdCardFontScale(effectiveFontId);
  if (scale != 256) adv = (adv * scale + 128) >> 8;
  return adv;
}

int GfxRenderer::getSpaceAdvance(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                                 const EpdFontFamily::Style style) const {
  // Advance table fast-path for SD card fonts during layout.
  // Kern data is not loaded during layout (consistent with previous metadataOnly behavior),
  // so we return just the space advance without kerning.
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    const int32_t advFP = sdIt->second->getAdvance(' ', static_cast<uint8_t>(style));
    const uint16_t scale = getSdCardFontScale(fontId);
    if (scale != 256) {
      return fp4::toPixel(static_cast<int32_t>(static_cast<int64_t>(advFP) * scale / 256));
    }
    return fp4::toPixel(advFP);
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const auto& font = fontIt->second;
  const EpdGlyph* spaceGlyph = font.getGlyph(' ', style);
  const int32_t spaceAdvanceFP = spaceGlyph ? static_cast<int32_t>(spaceGlyph->advanceX) : 0;
  // Combine space advance + flanking kern into one fixed-point sum before snapping.
  // Snapping the combined value avoids the +/-1 px error from snapping each component separately.
  const int32_t kernFP = static_cast<int32_t>(font.getKerning(leftCp, ' ', style)) +
                         static_cast<int32_t>(font.getKerning(' ', rightCp, style));
  return fp4::toPixel(spaceAdvanceFP + kernFP);
}

int GfxRenderer::getKerning(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                            const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const int kernFP = fontIt->second.getKerning(leftCp, rightCp, style);  // 4.4 fixed-point
  int kern = fp4::toPixel(kernFP);
  const uint16_t scale = getSdCardFontScale(fontId);
  if (scale != 256) kern = (kern * static_cast<int>(scale) + 128) >> 8;
  return kern;
}

int GfxRenderer::getTextAdvanceX(const int fontId, const char* text, EpdFontFamily::Style style) const {
  // Advance table fast-path for SD card fonts during layout.
  // No kerning/ligature lookup — consistent with previous metadataOnly behavior
  // where kern/lig data was not loaded.
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    int32_t widthFP = 0;
    const uint8_t styleIdx = static_cast<uint8_t>(style);
    while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
      widthFP += sdIt->second->getAdvance(cp, styleIdx);
    }
    const uint16_t scale = getSdCardFontScale(fontId);
    if (scale != 256) {
      return fp4::toPixel(static_cast<int32_t>(static_cast<int64_t>(widthFP) * scale / 256));
    }
    return fp4::toPixel(widthFP);
  }

  // External reader font: compute advance using bitmap font metrics
  if (isReaderFont(fontId)) {
    FontManager& fm = FontManager::getInstance();
    if (fm.isExternalFontEnabled()) {
      ExternalFont* extFont = fm.getActiveFont();
      if (extFont) {
        int width = 0;
        const int effectiveFontId = getEffectiveFontId(fontId);
        const auto fallbackIt = fontMap.find(effectiveFontId);
        const int cjkAdvance = clampExternalAdvance(extFont->getCharWidth(), cjkSpacing);
        uint32_t cp;
        while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
          if (utf8IsCombiningMark(cp)) continue;
          // CJK: use charWidth directly (no SD card read needed)
          if (isCjkCodepoint(cp)) {
            width += cjkAdvance;
            continue;
          }
          // Non-CJK: try external font glyph metrics
          const uint8_t* bitmap = extFont->getGlyph(cp);
          if (bitmap) {
            uint8_t advanceX = extFont->getCharWidth();
            extFont->getGlyphMetrics(cp, nullptr, &advanceX);
            int spacing = 0;
            if (isAsciiDigit(cp)) {
              spacing = asciiDigitSpacing;
            } else if (isAsciiLetter(cp)) {
              spacing = asciiLetterSpacing;
            }
            width += clampExternalAdvance(advanceX, spacing);
          } else if (fallbackIt != fontMap.end()) {
            // Fall back to built-in reader font for missing glyphs
            const EpdGlyph* glyph = fallbackIt->second.getGlyph(cp, style);
            if (glyph) width += glyph->advanceX;
          }
        }
        return width;
      }
    }
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  uint32_t cp;
  uint32_t prevCp = 0;
  int32_t widthFP = 0;  // 12.4 fixed-point accumulator
  const auto& font = fontIt->second;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) {
      continue;
    }
    cp = font.applyLigatures(cp, text, style);
    if (prevCp != 0) {
      widthFP += font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
    }
    const EpdGlyph* glyph = font.getGlyph(cp, style);
    if (glyph) widthFP += glyph->advanceX;  // 12.4 fixed-point advance
    prevCp = cp;
  }
  return fp4::toPixel(widthFP);  // snap 12.4 fixed-point to nearest pixel
}

int GfxRenderer::getFontAscenderSize(const int fontId) const {
  // Check if using external font for reader fonts
  if (isReaderFont(fontId)) {
    FontManager& fm = FontManager::getInstance();
    if (fm.isExternalFontEnabled()) {
      ExternalFont* extFont = fm.getActiveFont();
      if (extFont) {
        // For external fonts, use charHeight as ascender
        return extFont->getCharHeight();
      }
    }
  }

  const int effectiveFontId = getEffectiveFontId(fontId);
  if (fontMap.count(effectiveFontId) == 0) {
    // UI fonts may not be in fontMap (they use built-in CJK font)
    if (isUiFont(fontId)) {
      return CjkUiFont20::CJK_UI_FONT_HEIGHT;
    }
    LOG_ERR("GFX", "Font %d not found", effectiveFontId);
    return 0;
  }

  return fontMap.at(effectiveFontId).getData(EpdFontFamily::REGULAR)->ascender;
}

int GfxRenderer::getLineHeight(const int fontId) const {
  // Check if using external font for reader fonts
  if (isReaderFont(fontId)) {
    FontManager& fm = FontManager::getInstance();
    if (fm.isExternalFontEnabled()) {
      ExternalFont* extFont = fm.getActiveFont();
      if (extFont) {
        // charHeight is the line spacing defined by the font
        return extFont->getCharHeight();
      }
    }
  }

  const int effectiveFontId = getEffectiveFontId(fontId);
  if (fontMap.count(effectiveFontId) == 0) {
    // UI fonts may not be in fontMap (they use built-in CJK font)
    if (isUiFont(fontId)) {
      return CjkUiFont20::CJK_UI_FONT_HEIGHT + 4;  // 20px + 4px spacing
    }
    LOG_ERR("GFX", "Font %d not found", effectiveFontId);
    return 0;
  }

  return fontMap.at(effectiveFontId).getData(EpdFontFamily::REGULAR)->advanceY;
}

int GfxRenderer::getTextHeight(const int fontId) const {
  const int effectiveFontId = getEffectiveFontId(fontId);
  if (fontMap.count(effectiveFontId) == 0) {
    // UI fonts may not be in fontMap (they use built-in CJK font)
    if (isUiFont(fontId)) {
      return CjkUiFont20::CJK_UI_FONT_HEIGHT;  // 20px for CJK UI font
    }
    LOG_ERR("GFX", "Font %d not found", effectiveFontId);
    return 0;
  }
  return fontMap.at(effectiveFontId).getData(EpdFontFamily::REGULAR)->ascender;
}

void GfxRenderer::drawTextVertical(const int fontId, const int x, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style) const {
  if (text == nullptr || *text == '\0') return;

  // Scan mode: record text for prewarm
  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    fontCacheManager_->recordText(text, fontId, style);
    return;
  }

  const int effectiveFontId = getEffectiveFontId(fontId);
  if (fontMap.count(effectiveFontId) == 0) {
    LOG_ERR("GFX", "drawTextVertical: font %d not found", effectiveFontId);
    return;
  }

  const auto& font = fontMap.at(effectiveFontId);
  int yPos = y;
  const char* ptr = text;

  while (*ptr) {
    uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&ptr));
    if (cp == 0) break;

    const EpdGlyph* glyph = font.getGlyph(cp, style);
    if (!glyph) continue;

    const int advance = fp4::toPixel(glyph->advanceX);
    // Add 10% vertical spacing between characters (equivalent to line spacing in horizontal mode)
    const int verticalAdvance = advance + advance / 10;
    const auto* punctOffset = VerticalTextUtils::getVerticalPunctuationOffset(cp);

    // Re-encode codepoint to UTF-8 for single-char drawing
    char charBuf[5] = {};
    if (cp < 0x80) {
      charBuf[0] = static_cast<char>(cp);
    } else if (cp < 0x800) {
      charBuf[0] = static_cast<char>(0xC0 | (cp >> 6));
      charBuf[1] = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      charBuf[0] = static_cast<char>(0xE0 | (cp >> 12));
      charBuf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      charBuf[2] = static_cast<char>(0x80 | (cp & 0x3F));
    }

    // All characters drawn upright for now.
    // TODO: Use OpenType 'vert' feature for proper vertical glyph variants
    drawText(effectiveFontId, x, yPos, charBuf, black, style);
    yPos += verticalAdvance;
  }
}

void GfxRenderer::drawTextRotated90CW(const int fontId, const int x, const int y, const char* text, const bool black,
                                      const EpdFontFamily::Style style) const {
  // Cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  const int effectiveFontId = getEffectiveFontId(fontId);
  if (fontMap.count(effectiveFontId) == 0) {
    // UI fonts may not be in fontMap (they use built-in CJK font)
    if (isUiFont(fontId)) {
      // Render using built-in CJK UI font directly (rotated)
      int yPos = y;  // Current Y position (decreases as we draw characters)
      const char* ptr = text;
      uint32_t cp;
      while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&ptr)))) {
        if (CjkUiFont20::hasCjkUiGlyph(cp)) {
          const uint8_t* bitmap = CjkUiFont20::getCjkUiGlyph(cp);
          const uint8_t width = CjkUiFont20::getCjkUiGlyphWidth(cp);
          const uint8_t height = CjkUiFont20::CJK_UI_FONT_HEIGHT;
          const uint8_t bytesPerRow = CjkUiFont20::CJK_UI_FONT_BYTES_PER_ROW;

          // For 90 clockwise rotation: (glyphX, glyphY) -> (glyphY, -glyphX)
          const int startX = x;
          for (uint8_t glyphY = 0; glyphY < height; glyphY++) {
            const int screenX = startX + glyphY;
            for (uint8_t glyphX = 0; glyphX < width; glyphX++) {
              const uint8_t byteIndex = glyphY * bytesPerRow + (glyphX / 8);
              const uint8_t bitIndex = 7 - (glyphX % 8);
              const uint8_t byte = pgm_read_byte(&bitmap[byteIndex]);
              if ((byte >> bitIndex) & 1) {
                const int screenY = yPos - glyphX;
                drawPixel(screenX, screenY, black);
              }
            }
          }
          yPos -= width;
        } else {
          yPos -= 10;  // Default width for unknown characters
        }
      }
      return;
    }
    LOG_ERR("GFX", "Font %d not found", effectiveFontId);
    return;
  }
  const auto& font = fontMap.at(effectiveFontId);

  // No printable characters
  if (!fontHasPrintableChars(font, text, style)) {
    if (isReaderFont(fontId)) {
      return;
    }
    if (!hasUiGlyphForText(text)) {
      return;
    }
  }

  // For 90 clockwise rotation:
  // Original (glyphX, glyphY) -> Rotated (glyphY, -glyphX)
  // Text reads from bottom to top

  int yPos = y;  // Current Y position (decreases as we draw characters)

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    // For ASCII characters, prefer EPD font (better quality for Latin text)
    // Only use CJK UI font for non-ASCII characters or when EPD font lacks the
    // glyph
    bool useEpdFont = (cp < 0x80) && font.getGlyph(cp, style) != nullptr;

    if (!isReaderFont(fontId) && !useEpdFont) {
      const uint8_t* bitmap = nullptr;
      uint8_t fontWidth = 0;
      uint8_t fontHeight = 0;
      uint8_t bytesPerRow = 0;
      uint8_t bytesPerChar = 0;
      uint8_t advance = 0;

      if (CjkUiFont20::hasCjkUiGlyph(cp)) {
        bitmap = CjkUiFont20::getCjkUiGlyph(cp);
        fontWidth = CjkUiFont20::CJK_UI_FONT_WIDTH;
        fontHeight = CjkUiFont20::CJK_UI_FONT_HEIGHT;
        bytesPerRow = CjkUiFont20::CJK_UI_FONT_BYTES_PER_ROW;
        bytesPerChar = CjkUiFont20::CJK_UI_FONT_BYTES_PER_CHAR;
        advance = CjkUiFont20::getCjkUiGlyphWidth(cp);
      }

      if (bitmap && advance > 0) {
        bool hasContent = false;
        for (int i = 0; i < bytesPerChar; i++) {
          if (pgm_read_byte(&bitmap[i]) != 0) {
            hasContent = true;
            break;
          }
        }

        if (hasContent) {
          const int startX = x;

          for (int glyphY = 0; glyphY < fontHeight; glyphY++) {
            const int screenX = startX + glyphY;
            for (int glyphX = 0; glyphX < fontWidth; glyphX++) {
              const int byteIndex = glyphY * bytesPerRow + (glyphX / 8);
              const int bitIndex = 7 - (glyphX % 8);
              const uint8_t byte = pgm_read_byte(&bitmap[byteIndex]);
              if ((byte >> bitIndex) & 1) {
                const int screenY = yPos - glyphX;
                drawPixel(screenX, screenY, black);
              }
            }
          }
        }

        yPos -= std::max(1, static_cast<int>(advance));
        continue;
      }
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);
    if (!glyph) {
      glyph = font.getGlyph('?', style);
    }
    if (!glyph) {
      continue;
    }

    const EpdFontData* fontData = font.getData(style);
    const int is2Bit = fontData->is2Bit;
    const uint8_t width = glyph->width;
    const uint8_t height = glyph->height;
    const int left = glyph->left;
    const int top = glyph->top;

    const uint8_t* bitmap = getGlyphBitmap(fontData, glyph);

    if (bitmap != nullptr) {
      for (int glyphY = 0; glyphY < height; glyphY++) {
        for (int glyphX = 0; glyphX < width; glyphX++) {
          const int pixelPosition = glyphY * width + glyphX;

          // 90 clockwise rotation transformation:
          // screenX = x + (ascender - top + glyphY)
          // screenY = yPos - (left + glyphX)
          const int screenX = x + (fontData->ascender - top + glyphY);
          const int screenY = yPos - left - glyphX;

          if (is2Bit) {
            const uint8_t byte = bitmap[pixelPosition / 4];
            const uint8_t bit_index = (3 - pixelPosition % 4) * 2;
            const uint8_t bmpVal = 3 - ((byte >> bit_index) & 0x3);

            if (renderMode == BW && bmpVal < 3) {
              drawPixel(screenX, screenY, black);
            } else if (renderMode == GRAYSCALE_MSB && (bmpVal == 1 || bmpVal == 2)) {
              drawPixel(screenX, screenY, false);
            } else if (renderMode == GRAYSCALE_LSB && bmpVal == 1) {
              drawPixel(screenX, screenY, false);
            }
          } else {
            const uint8_t byte = bitmap[pixelPosition / 8];
            const uint8_t bit_index = 7 - (pixelPosition % 8);

            if ((byte >> bit_index) & 1) {
              drawPixel(screenX, screenY, black);
            }
          }
        }
      }
    }

    // Move to next character position (going up, so decrease Y)
    yPos -= glyph->advanceX;
  }
}

void GfxRenderer::drawButtonHints(const int fontId, const char* btn1, const char* btn2, const char* btn3,
                                  const char* btn4) {
  const Orientation orientation = getOrientation();
  const int pageHeight = getScreenHeight();
  const int pageWidth = getScreenWidth();
  constexpr int buttonWidth = BUTTON_HINT_WIDTH;
  constexpr int buttonHeight = BUTTON_HINT_HEIGHT;
  constexpr int buttonY = BUTTON_HINT_BOTTOM_INSET;  // Distance from bottom
  constexpr int textYOffset = BUTTON_HINT_TEXT_OFFSET;
  constexpr int buttonPositions[] = {25, 130, 245, 350};
  const char* labels[] = {btn1, btn2, btn3, btn4};
  const bool isLandscape =
      orientation == Orientation::LandscapeClockwise || orientation == Orientation::LandscapeCounterClockwise;
  const bool placeAtTop = orientation == Orientation::PortraitInverted;
  const int buttonTop = placeAtTop ? 0 : pageHeight - buttonY;

  if (isLandscape) {
    const bool placeLeft = orientation == Orientation::LandscapeClockwise;
    const int buttonLeft = placeLeft ? 0 : pageWidth - buttonWidth;
    if (orientation == Orientation::LandscapeCounterClockwise) {
      const char* tmp = labels[0];
      labels[0] = labels[3];
      labels[3] = tmp;
      tmp = labels[1];
      labels[1] = labels[2];
      labels[2] = tmp;
    }
    for (int i = 0; i < 4; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = buttonPositions[i];
        fillRect(buttonLeft, y, buttonWidth, buttonHeight, false);
        drawRect(buttonLeft, y, buttonWidth, buttonHeight);
        const int textWidth = getTextWidth(fontId, labels[i]);
        const int textX = buttonLeft + (buttonWidth - 1 - textWidth) / 2;
        drawText(fontId, textX, y + textYOffset, labels[i]);
      }
    }
    return;
  }

  for (int i = 0; i < 4; i++) {
    // Only draw if the label is non-empty
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[i];
      fillRect(x, buttonTop, buttonWidth, buttonHeight, false);
      drawRect(x, buttonTop, buttonWidth, buttonHeight);
      const int textWidth = getTextWidth(fontId, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      drawText(fontId, textX, buttonTop + textYOffset, labels[i]);
    }
  }
}

void GfxRenderer::drawSideButtonHints(const int fontId, const char* topBtn, const char* bottomBtn) const {
  const Orientation orientation = getOrientation();
  const int screenWidth = getScreenWidth();
  constexpr int buttonWidth = 40;   // Width on screen (height when rotated)
  constexpr int buttonHeight = 80;  // Height on screen (width when rotated)
  constexpr int buttonX = 5;        // Distance from right edge
  // Position for the button group - buttons share a border so they're adjacent
  constexpr int topButtonY = 345;  // Top button position

  const char* labels[] = {topBtn, bottomBtn};

  // Draw the shared border for both buttons as one unit
  const bool placeLeft = orientation == Orientation::PortraitInverted;
  const int x = placeLeft ? buttonX : screenWidth - buttonX - buttonWidth;
  const int y = topButtonY;

  // Draw top button outline (3 sides, bottom open)
  if (topBtn != nullptr && topBtn[0] != '\0') {
    drawLine(x, y, x + buttonWidth - 1, y);   // Top
    drawLine(x, y, x, y + buttonHeight - 1);  // Left
    drawLine(x + buttonWidth - 1, y, x + buttonWidth - 1,
             y + buttonHeight - 1);  // Right
  }

  // Draw shared middle border
  if ((topBtn != nullptr && topBtn[0] != '\0') || (bottomBtn != nullptr && bottomBtn[0] != '\0')) {
    drawLine(x, y + buttonHeight, x + buttonWidth - 1,
             y + buttonHeight);  // Shared border
  }

  // Draw bottom button outline (3 sides, top is shared)
  if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
    drawLine(x, y + buttonHeight, x,
             y + 2 * buttonHeight - 1);  // Left
    drawLine(x + buttonWidth - 1, y + buttonHeight, x + buttonWidth - 1,
             y + 2 * buttonHeight - 1);  // Right
    drawLine(x, y + 2 * buttonHeight - 1, x + buttonWidth - 1,
             y + 2 * buttonHeight - 1);  // Bottom
  }

  // Draw text for each button
  for (int i = 0; i < 2; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int yPos = y + i * buttonHeight;
      const char* label = labels[i];

      // Draw rotated text centered in the button
      const int textWidth = getTextWidth(fontId, label);
      int textHeight = getTextHeight(fontId);
      bool hasCjk = false;
      const char* scan = label;
      uint32_t cp;
      while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&scan)))) {
        if (isCjkCodepoint(cp)) {
          hasCjk = true;
          break;
        }
      }
      if (hasCjk) {
        textHeight = CjkUiFont20::CJK_UI_FONT_HEIGHT;
      }

      // Center the rotated text in the button
      const int textX = x + (buttonWidth - textHeight) / 2;
      const int textY = yPos + (buttonHeight + textWidth) / 2;

      drawTextRotated90CW(fontId, textX, textY, label);
    }
  }
}

uint8_t* GfxRenderer::getFrameBuffer() const { return frameBuffer; }

size_t GfxRenderer::getBufferSize() const { return frameBufferSize; }

// unused
// void GfxRenderer::grayscaleRevert() const { display.grayscaleRevert(); }

void GfxRenderer::copyGrayscaleLsbBuffers() const { display.copyGrayscaleLsbBuffers(frameBuffer); }

void GfxRenderer::copyGrayscaleMsbBuffers() const { display.copyGrayscaleMsbBuffers(frameBuffer); }

void GfxRenderer::displayGrayBuffer(const bool turnOffScreen, const bool darkMode) const {
  // Note: HalDisplay::displayGrayBuffer does not support darkMode parameter directly.
  // Dark mode grayscale rendering is handled at the pixel level in renderChar.
  display.displayGrayBuffer(turnOffScreen);
}

void GfxRenderer::freeBwBufferChunks() {
  for (auto& bwBufferChunk : bwBufferChunks) {
    if (bwBufferChunk) {
      free(bwBufferChunk);
      bwBufferChunk = nullptr;
    }
  }
}

/**
 * This should be called before grayscale buffers are populated.
 * A `restoreBwBuffer` call should always follow the grayscale render if this method was called.
 * Uses chunked allocation to avoid needing 48KB of contiguous memory.
 * Returns true if buffer was stored successfully, false if allocation failed.
 */
bool GfxRenderer::storeBwBuffer() {
  // Allocate and copy each chunk
  for (size_t i = 0; i < bwBufferChunks.size(); i++) {
    // Check if any chunks are already allocated
    if (bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! BW buffer chunk %zu already stored - this is likely a bug, freeing chunk", i);
      free(bwBufferChunks[i]);
      bwBufferChunks[i] = nullptr;
    }

    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, static_cast<size_t>(frameBufferSize - offset));
    bwBufferChunks[i] = static_cast<uint8_t*>(malloc(chunkSize));

    if (!bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! Failed to allocate BW buffer chunk %zu (%zu bytes)", i, chunkSize);
      // Free previously allocated chunks
      freeBwBufferChunks();
      return false;
    }

    memcpy(bwBufferChunks[i], frameBuffer + offset, chunkSize);
  }

  LOG_DBG("GFX", "Stored BW buffer in %zu chunks (%zu bytes each)", bwBufferChunks.size(), BW_BUFFER_CHUNK_SIZE);
  return true;
}

/**
 * This can only be called if `storeBwBuffer` was called prior to the grayscale render.
 * It should be called to restore the BW buffer state after grayscale rendering is complete.
 * Uses chunked restoration to match chunked storage.
 */
void GfxRenderer::restoreBwBuffer() {
  // Check if all chunks are allocated
  bool missingChunks = false;
  for (const auto& bwBufferChunk : bwBufferChunks) {
    if (!bwBufferChunk) {
      missingChunks = true;
      break;
    }
  }

  if (missingChunks) {
    freeBwBufferChunks();
    return;
  }

  for (size_t i = 0; i < bwBufferChunks.size(); i++) {
    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, static_cast<size_t>(frameBufferSize - offset));
    memcpy(frameBuffer + offset, bwBufferChunks[i], chunkSize);
  }

  display.cleanupGrayscaleBuffers(frameBuffer);

  freeBwBufferChunks();
  LOG_DBG("GFX", "Restored and freed BW buffer chunks");
}

/**
 * Cleanup grayscale buffers using the current frame buffer.
 * Use this when BW buffer was re-rendered instead of stored/restored.
 */
void GfxRenderer::cleanupGrayscaleWithFrameBuffer() const {
  if (frameBuffer) {
    display.cleanupGrayscaleBuffers(frameBuffer);
  }
}

void GfxRenderer::renderChar(const int fontId, const EpdFontFamily& fontFamily, const uint32_t cp, int* x, const int* y,
                             const bool pixelState, const EpdFontFamily::Style style) const {
  FontManager& fm = FontManager::getInstance();

  // Cache character classification results to avoid repeated calls (perf opt)
  const bool isCjk = isCjkCodepoint(cp);

  // Prefer external reader font when enabled; fall back to built-in only if missing
  if (isReaderFont(fontId)) {
    if (fm.isExternalFontEnabled()) {
      ExternalFont* extFont = fm.getActiveFont();
      if (extFont) {
        const uint8_t* bitmap = extFont->getGlyph(cp);
        if (bitmap) {
          uint8_t minX = 0, advanceX = extFont->getCharWidth();
          extFont->getGlyphMetrics(cp, &minX, &advanceX);
          int spacing = 0;
          if (isCjk) {
            spacing = cjkSpacing;
          } else if (isAsciiDigit(cp)) {
            spacing = asciiDigitSpacing;
          } else if (isAsciiLetter(cp)) {
            spacing = asciiLetterSpacing;
          }
          int advance = clampExternalAdvance(advanceX, spacing);
          renderExternalGlyph(bitmap, extFont, x, *y, pixelState, advance, minX);
          return;
        }
        // Missing glyph in external font - try built-in CJK UI font for CJK
        if (isCjk && CjkUiFont20::hasCjkUiGlyph(cp)) {
          renderBuiltinCjkGlyph(cp, x, *y, pixelState);
          return;
        }
        // Fall through to built-in reader font rendering below
      }
    }
  } else {
    // UI font - for CJK characters, prioritize built-in UI font (Flash, fast)
    // Only fall back to external font if built-in doesn't have the glyph
    if (isCjk) {
      // First check built-in CJK UI font (Flash access is fast)
      if (CjkUiFont20::hasCjkUiGlyph(cp)) {
        renderBuiltinCjkGlyph(cp, x, *y, pixelState);
        return;
      }

      // Built-in doesn't have this glyph - try external UI font (SD card, slow)
      if (fm.isUiFontEnabled()) {
        ExternalFont* uiExtFont = fm.getActiveUiFont();
        if (uiExtFont) {
          const uint8_t* bitmap = uiExtFont->getGlyph(cp);
          if (bitmap) {
            uint8_t minX = 0, advanceX = 0;
            uiExtFont->getGlyphMetrics(cp, &minX, &advanceX);
            renderExternalGlyph(bitmap, uiExtFont, x, *y, pixelState, advanceX, minX);
            return;
          }
        }
      }

      // Last resort: try reader font if UI font doesn't have this glyph
      if (fm.isExternalFontEnabled()) {
        ExternalFont* extFont = fm.getActiveFont();
        if (extFont) {
          const uint8_t* bitmap = extFont->getGlyph(cp);
          if (bitmap) {
            uint8_t minX = 0, advanceX = 0;
            extFont->getGlyphMetrics(cp, &minX, &advanceX);
            renderExternalGlyph(bitmap, extFont, x, *y, pixelState, advanceX, minX);
            return;
          }
        }
      }
    } else {
      // Non-CJK characters in UI - check built-in UI font first
      if (CjkUiFont20::hasCjkUiGlyph(cp)) {
        renderBuiltinCjkGlyph(cp, x, *y, pixelState);
        return;
      }
      // Then try external UI font if enabled
      if (fm.isUiFontEnabled()) {
        ExternalFont* uiExtFont = fm.getActiveUiFont();
        if (uiExtFont) {
          const uint8_t* bitmap = uiExtFont->getGlyph(cp);
          if (bitmap) {
            uint8_t minX = 0, advanceX = 0;
            uiExtFont->getGlyphMetrics(cp, &minX, &advanceX);
            renderExternalGlyph(bitmap, uiExtFont, x, *y, pixelState, advanceX, minX);
            return;
          }
        }
      }
      // Fall through to EPD font rendering below
    }
  }

  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) {
    glyph = fontFamily.getGlyph('?', style);
  }

  // no glyph? Still advance cursor to prevent overlap
  if (!glyph) {
    LOG_ERR("GFX", "No glyph for codepoint %d", cp);
    if (isCjk) {
      *x += 20;  // Full-width character default
    } else {
      *x += 10;  // Half-width character default
    }
    return;
  }

  const EpdFontData* fontData = fontFamily.getData(style);
  const bool is2Bit = fontData->is2Bit;
  const uint8_t baseW = glyph->width;
  const uint8_t baseH = glyph->height;

  const uint8_t* bitmap = getGlyphBitmap(fontData, glyph);

  // SD card font scaling
  const uint16_t scale = getSdCardFontScale(fontId);
  const bool needsScale = (scale != 256);
  const int drawW = needsScale ? ((baseW * scale + 128) >> 8) : baseW;
  const int drawH = needsScale ? ((baseH * scale + 128) >> 8) : baseH;
  const int drawLeft = needsScale ? ((glyph->left * static_cast<int>(scale) + 128) >> 8) : glyph->left;
  const int drawTop = needsScale ? ((glyph->top * static_cast<int>(scale) + 128) >> 8) : glyph->top;

  // Synthetic bold: Bold requested but font has no separate Bold style (same data as Regular)
  const bool synthBold = (style & EpdFontFamily::BOLD) &&
                         sdCardFontScales_.count(fontId) > 0 &&
                         fontFamily.getData(style) == fontFamily.getData(EpdFontFamily::REGULAR);
  const int boldPasses = synthBold ? 2 : 1;

  if (bitmap != nullptr) {
    for (int pass = 0; pass < boldPasses; pass++) {
      const int xBoldOffset = pass;  // 2nd pass: 1px right shift for bold
      for (int glyphY = 0; glyphY < drawH; glyphY++) {
        const int srcY = needsScale ? (glyphY * 256 / scale) : glyphY;
        if (srcY >= baseH) continue;
        const int screenY = *y - drawTop + glyphY;
        for (int glyphX = 0; glyphX < drawW; glyphX++) {
          const int srcX = needsScale ? (glyphX * 256 / scale) : glyphX;
          if (srcX >= baseW) continue;

          const int pixelPosition = srcY * baseW + srcX;
          const int screenX = *x + drawLeft + glyphX + xBoldOffset;

          if (is2Bit) {
            const uint8_t byte = bitmap[pixelPosition / 4];
            const uint8_t bit_index = (3 - pixelPosition % 4) * 2;
            const uint8_t bmpVal = 3 - ((byte >> bit_index) & 0x3);

            if (renderMode == BW) {
              bool shouldDraw = false;
              if (darkMode) {
                if (bmpVal == 0) {
                  shouldDraw = true;
                }
              } else if (bmpVal < 3) {
                shouldDraw = true;
              }

              if (shouldDraw) {
                drawPixel(screenX, screenY, pixelState);
              }
            } else if (renderMode == GRAYSCALE_MSB || renderMode == GRAYSCALE_LSB) {
              if (bmpVal < 3) {
                uint8_t val = bmpVal;
                if (darkMode) {
                  val = 3 - val;
                }
                bool bit = (renderMode == GRAYSCALE_LSB) ? (val & 1) : ((val >> 1) & 1);
                drawPixel(screenX, screenY, !bit);
              }
            }
          } else {
            const uint8_t byte = bitmap[pixelPosition / 8];
            const uint8_t bit_index = 7 - (pixelPosition % 8);

            if ((byte >> bit_index) & 1) {
              drawPixel(screenX, screenY, pixelState);
            }
          }
        }
      }
    }
  }

  int advPx = fp4::toPixel(glyph->advanceX);
  if (needsScale) advPx = (advPx * scale + 128) >> 8;
  if (synthBold) advPx += 1;
  *x += advPx;
}

void GfxRenderer::getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const {
  switch (orientation) {
    case Portrait:
      *outTop = VIEWABLE_MARGIN_TOP;
      *outRight = VIEWABLE_MARGIN_RIGHT;
      *outBottom = VIEWABLE_MARGIN_BOTTOM;
      *outLeft = VIEWABLE_MARGIN_LEFT;
      break;
    case LandscapeClockwise:
      *outTop = VIEWABLE_MARGIN_LEFT;
      *outRight = VIEWABLE_MARGIN_TOP;
      *outBottom = VIEWABLE_MARGIN_RIGHT;
      *outLeft = VIEWABLE_MARGIN_BOTTOM;
      break;
    case PortraitInverted:
      *outTop = VIEWABLE_MARGIN_BOTTOM;
      *outRight = VIEWABLE_MARGIN_LEFT;
      *outBottom = VIEWABLE_MARGIN_TOP;
      *outLeft = VIEWABLE_MARGIN_RIGHT;
      break;
    case LandscapeCounterClockwise:
      *outTop = VIEWABLE_MARGIN_RIGHT;
      *outRight = VIEWABLE_MARGIN_BOTTOM;
      *outBottom = VIEWABLE_MARGIN_LEFT;
      *outLeft = VIEWABLE_MARGIN_TOP;
      break;
  }
}

// Check if fontId is a reader font (should use external Chinese font or SD card font rendering path).
// UI fonts (UI_10, UI_12, SMALL_FONT) should NOT use external font.
bool GfxRenderer::isReaderFont(const int fontId) const {
  // SD card fonts are always reader fonts — their IDs are computed via FNV-1a
  // hash and can be positive or negative. Without this check, positive SD card
  // font IDs fall through to the "UI font" branch in renderChar(), causing CJK
  // characters to be drawn by CjkUiFont20 (fixed 20px) instead of the SD card
  // font's own glyphs.
  if (sdCardFonts_.count(fontId) > 0) {
    return true;
  }

  // First check if it's a UI font - UI fonts should NOT use external reader
  // font
  for (int i = 0; i < UI_FONT_COUNT; i++) {
    if (UI_FONT_IDS[i] == fontId) {
      return false;  // This is a UI font, not a reader font
    }
  }

  // External font IDs are negative (from CrossPointSettings::getReaderFontId())
  if (fontId < 0) {
    return true;
  }

  for (int i = 0; i < READER_FONT_COUNT; i++) {
    if (READER_FONT_IDS[i] == fontId) {
      return true;
    }
  }
  return false;
}

// Get effective font ID, handling fallback for external reader font IDs
// When external font is selected (negative ID) but not available, fall back to
// built-in font
int GfxRenderer::getEffectiveFontId(int fontId) const {
  // Only negative IDs that are reader fonts need fallback
  // UI fonts have negative IDs too but should not be redirected
  if (fontId < 0 && isReaderFont(fontId)) {
    // SD card fonts are registered in fontMap — use them directly
    if (fontMap.count(fontId) != 0) {
      return fontId;
    }
    // ExternalFont ID not in fontMap — fall back to built-in reader font
    return readerFallbackFontId != 0 ? readerFallbackFontId : READER_FONT_IDS[0];
  }
  return fontId;
}

void GfxRenderer::renderExternalGlyph(const uint8_t* bitmap, ExternalFont* font, int* x, const int y,
                                      const bool pixelState, const int advanceOverride, const int minX) const {
  const uint8_t width = font->getCharWidth();
  const uint8_t height = font->getCharHeight();
  const uint8_t bytesPerRow = font->getBytesPerRow();

  // Calculate starting Y position (baseline alignment)
  const int startY = y - height + 4;  // +4 is baseline adjustment

  // Only render pixels from minX onwards, drawn at screen position *x
  // This trims the left-side empty space so advanceX matches the drawn width
  for (int glyphY = 0; glyphY < height; glyphY++) {
    const int screenY = startY + glyphY;
    for (int glyphX = minX; glyphX < width; glyphX++) {
      const int byteIndex = glyphY * bytesPerRow + (glyphX / 8);
      const int bitIndex = 7 - (glyphX % 8);  // MSB first

      if ((bitmap[byteIndex] >> bitIndex) & 1) {
        drawPixel(*x + (glyphX - minX), screenY, pixelState);
      }
    }
  }

  // Advance cursor
  const int advance = (advanceOverride >= 0) ? advanceOverride : width;
  *x += std::max(1, advance);
}

void GfxRenderer::renderBuiltinCjkGlyph(const uint32_t cp, int* x, const int y, const bool pixelState) const {
  // Use built-in 20px CJK UI font
  const uint8_t* bitmap = CjkUiFont20::getCjkUiGlyph(cp);
  const uint8_t fontWidth = CjkUiFont20::CJK_UI_FONT_WIDTH;
  const uint8_t fontHeight = CjkUiFont20::CJK_UI_FONT_HEIGHT;
  const uint8_t bytesPerRow = CjkUiFont20::CJK_UI_FONT_BYTES_PER_ROW;
  const uint8_t bytesPerChar = CjkUiFont20::CJK_UI_FONT_BYTES_PER_CHAR;
  const uint8_t actualWidth = CjkUiFont20::getCjkUiGlyphWidth(cp);

  if (!bitmap || actualWidth == 0) {
    return;
  }

  // Check if glyph is empty (all zeros) - skip rendering but still advance
  // cursor
  bool hasContent = false;
  for (int i = 0; i < bytesPerChar; i++) {
    if (pgm_read_byte(&bitmap[i]) != 0) {
      hasContent = true;
      break;
    }
  }

  if (hasContent) {
    const int startY = y - fontHeight + 4;  // 4px descent for CJK characters

    for (int glyphY = 0; glyphY < fontHeight; glyphY++) {
      const int screenY = startY + glyphY;
      for (int glyphX = 0; glyphX < fontWidth; glyphX++) {
        const int byteIndex = glyphY * bytesPerRow + (glyphX / 8);
        const int bitIndex = 7 - (glyphX % 8);  // MSB first

        // Read from PROGMEM
        const uint8_t byte = pgm_read_byte(&bitmap[byteIndex]);
        if ((byte >> bitIndex) & 1) {
          drawPixel(*x + glyphX, screenY, pixelState);
        }
      }
    }
  }

  // Advance cursor by actual width (proportional spacing)
  *x += actualWidth;
}
