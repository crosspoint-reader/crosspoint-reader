#include "GfxRenderer.h"
#include <EInkDisplay.h>
#include <EpdFontFamily.h>

#include <algorithm>
#include <FontManager.h>
#include <Utf8.h>

// Built-in CJK UI fonts (embedded in flash) - multiple sizes
#include "cjk_ui_font.h"    // 16x16 (legacy, not used)
#include "cjk_ui_font_20.h" // 20x20 (SMALL)
#include "cjk_ui_font_22.h" // 22x22 (MEDIUM)
#include "cjk_ui_font_24.h" // 24x24 (LARGE)

// Reader font IDs (from fontIds.h) - used to determine when to use external
// Chinese font UI fonts should NOT use external font
namespace {
// UI font IDs that should NOT use external reader font
constexpr int UI_FONT_IDS[] = {
    -1246724383, // UI_10_FONT_ID
    -359249323,  // UI_12_FONT_ID
    1073217904   // SMALL_FONT_ID
};
constexpr int UI_FONT_COUNT = sizeof(UI_FONT_IDS) / sizeof(UI_FONT_IDS[0]);

constexpr int READER_FONT_IDS[] = {
    -142329172,  // BOOKERLY_12_FONT_ID
    104246423,   // BOOKERLY_14_FONT_ID
    1909382491,  // BOOKERLY_16_FONT_ID
    2056549737,  // BOOKERLY_18_FONT_ID
    -1646794343, // NOTOSANS_12_FONT_ID
    -890242897,  // NOTOSANS_14_FONT_ID
    241925189,   // NOTOSANS_16_FONT_ID
    1503221336,  // NOTOSANS_18_FONT_ID
    875216341,   // OPENDYSLEXIC_8_FONT_ID
    -1234231183, // OPENDYSLEXIC_10_FONT_ID
    1682200414,  // OPENDYSLEXIC_12_FONT_ID
    -1851285286  // OPENDYSLEXIC_14_FONT_ID
};
constexpr int READER_FONT_COUNT =
    sizeof(READER_FONT_IDS) / sizeof(READER_FONT_IDS[0]);

// Check if a Unicode codepoint is CJK (Chinese/Japanese/Korean)
// Only these characters should use the external font width
bool isCjkCodepoint(const uint32_t cp) {
  // CJK Unified Ideographs: U+4E00 - U+9FFF
  if (cp >= 0x4E00 && cp <= 0x9FFF)
    return true;
  // CJK Unified Ideographs Extension A: U+3400 - U+4DBF
  if (cp >= 0x3400 && cp <= 0x4DBF)
    return true;
  // CJK Punctuation: U+3000 - U+303F
  if (cp >= 0x3000 && cp <= 0x303F)
    return true;
  // Hiragana: U+3040 - U+309F
  if (cp >= 0x3040 && cp <= 0x309F)
    return true;
  // Katakana: U+30A0 - U+30FF
  if (cp >= 0x30A0 && cp <= 0x30FF)
    return true;
  // CJK Compatibility Ideographs: U+F900 - U+FAFF
  if (cp >= 0xF900 && cp <= 0xFAFF)
    return true;
  // Fullwidth forms: U+FF00 - U+FFEF
  if (cp >= 0xFF00 && cp <= 0xFFEF)
    return true;
  // General Punctuation: U+2000 - U+206F (includes smart quotes, ellipsis,
  // dashes) Common Chinese punctuation: " " ' ' … — –
  if (cp >= 0x2000 && cp <= 0x206F)
    return true;
  return false;
}

bool isAsciiDigit(const uint32_t cp) { return cp >= '0' && cp <= '9'; }

bool isAsciiLetter(const uint32_t cp) {
  return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

int clampExternalAdvance(const int baseWidth, const int spacing) {
  return std::max(1, baseWidth + spacing);
}

bool hasUiGlyphForText(const char *text, const uint8_t uiFontSize) {
  if (text == nullptr || *text == '\0') {
    return false;
  }

  const char *ptr = text;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t **>(&ptr)))) {
    switch (uiFontSize) {
    case 0:
      if (CjkUiFont20::hasCjkUiGlyph(cp)) {
        return true;
      }
      break;
    case 1:
      if (CjkUiFont22::hasCjkUiGlyph(cp)) {
        return true;
      }
      break;
    case 2:
    default:
      if (CjkUiFont24::hasCjkUiGlyph(cp)) {
        return true;
      }
      break;
    }
  }
  return false;
}
} // namespace

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) {
  fontMap.insert({fontId, font});
}

void GfxRenderer::rotateCoordinates(const int x, const int y, int *rotatedX,
                                    int *rotatedY) const {
  switch (orientation) {
  case Portrait: {
    // Logical portrait (480x800) → panel (800x480)
    // Rotation: 90 degrees clockwise
    *rotatedX = y;
    *rotatedY = EInkDisplay::DISPLAY_HEIGHT - 1 - x;
    break;
  }
  case LandscapeClockwise: {
    // Logical landscape (800x480) rotated 180 degrees (swap top/bottom and
    // left/right)
    *rotatedX = EInkDisplay::DISPLAY_WIDTH - 1 - x;
    *rotatedY = EInkDisplay::DISPLAY_HEIGHT - 1 - y;
    break;
  }
  case PortraitInverted: {
    // Logical portrait (480x800) → panel (800x480)
    // Rotation: 90 degrees counter-clockwise
    *rotatedX = EInkDisplay::DISPLAY_WIDTH - 1 - y;
    *rotatedY = x;
    break;
  }
  case LandscapeCounterClockwise: {
    // Logical landscape (800x480) aligned with panel orientation
    *rotatedX = x;
    *rotatedY = y;
    break;
  }
  }
}

void GfxRenderer::drawPixel(const int x, const int y, const bool state) const {
  uint8_t *frameBuffer = einkDisplay.getFrameBuffer();

  // Early return if no framebuffer is set
  if (!frameBuffer) {
    Serial.printf("[%lu] [GFX] !! No framebuffer\n", millis());
    return;
  }

  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(x, y, &rotatedX, &rotatedY);

  // Bounds checking against physical panel dimensions
  if (rotatedX < 0 || rotatedX >= EInkDisplay::DISPLAY_WIDTH || rotatedY < 0 ||
      rotatedY >= EInkDisplay::DISPLAY_HEIGHT) {
    // Limit log output to avoid performance issues (log only first few per session)
    static int outsideRangeCount = 0;
    if (outsideRangeCount < 5) {
      Serial.printf("[%lu] [GFX] !! Outside range (%d, %d) -> (%d, %d)\n",
                    millis(), x, y, rotatedX, rotatedY);
      outsideRangeCount++;
      if (outsideRangeCount == 5) {
        Serial.printf("[GFX] !! Suppressing further outside range warnings\n");
      }
    }
    return;
  }

  // Calculate byte position and bit position
  const uint16_t byteIndex =
      rotatedY * EInkDisplay::DISPLAY_WIDTH_BYTES + (rotatedX / 8);
  const uint8_t bitPosition = 7 - (rotatedX % 8); // MSB first

  // In dark mode, invert the pixel state (black <-> white)
  // But NOT in grayscale mode - grayscale rendering uses special pixel marking
  // And NOT when skipDarkModeForImages is set - cover art should keep original
  // colors
  const bool shouldInvert =
      darkMode && !skipDarkModeForImages && renderMode == BW;
  const bool actualState = shouldInvert ? !state : state;

  if (actualState) {
    frameBuffer[byteIndex] &= ~(1 << bitPosition); // Clear bit = black pixel
  } else {
    frameBuffer[byteIndex] |= 1 << bitPosition; // Set bit = white pixel
  }
}

int GfxRenderer::getTextWidth(const int fontId, const char *text,
                              const EpdFontFamily::Style style) const {
  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }

  FontManager &fm = FontManager::getInstance();

  // Check if using external font for reader fonts
  if (isReaderFont(fontId)) {
    if (fm.isExternalFontEnabled()) {
      ExternalFont *extFont = fm.getActiveFont();
      if (extFont) {
        const int extCharWidth = extFont->getCharWidth();
        const int letterAdvance =
            clampExternalAdvance(extCharWidth, asciiLetterSpacing);
        const int digitAdvance =
            clampExternalAdvance(extCharWidth, asciiDigitSpacing);
        const int cjkAdvance =
            clampExternalAdvance(extCharWidth, cjkSpacing);

        // FAST PATH: Single CJK character (3-byte UTF-8, most common after CJK
        // splitting) Check: first byte is 0xE0-0xEF (3-byte UTF-8 lead),
        // followed by continuation bytes, then null
        const auto b0 = static_cast<unsigned char>(text[0]);
        if (b0 >= 0xE0 && b0 <= 0xEF && text[3] == '\0') {
          const auto b1 = static_cast<unsigned char>(text[1]);
          const auto b2 = static_cast<unsigned char>(text[2]);
          if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
            // This is a single 3-byte UTF-8 character
            // Decode codepoint to check if it's CJK
            const uint32_t cp =
                ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
            if (isCjkCodepoint(cp)) {
              return extCharWidth;
            }
          }
        }

        // Mixed width calculation: use external font width for CJK + ASCII
        // letters/digits, built-in width for the rest
        int width = 0;
        const char *ptr = text;
        const EpdFontFamily &fontFamily = fontMap.at(fontId);
        uint32_t cp;
        while ((
            cp = utf8NextCodepoint(reinterpret_cast<const uint8_t **>(&ptr)))) {
          if (isCjkCodepoint(cp)) {
            // CJK character: use external font width
            width += cjkAdvance;
          } else if (isAsciiDigit(cp)) {
            width += digitAdvance;
          } else if (isAsciiLetter(cp)) {
            width += letterAdvance;
          } else {
            // Non-CJK character: use built-in font width
            const EpdGlyph *glyph = fontFamily.getGlyph(cp, style);
            if (glyph) {
              width += glyph->advanceX;
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
    const char *checkPtr = text;
    bool hasUiFontChar = false;
    bool hasCjkChar = false;
    uint32_t testCp;
    while ((testCp = utf8NextCodepoint(
                reinterpret_cast<const uint8_t **>(&checkPtr)))) {
      bool inFont = false;
      switch (uiFontSize) {
      case 0:
        inFont = CjkUiFont20::hasCjkUiGlyph(testCp);
        break;
      case 1:
        inFont = CjkUiFont22::hasCjkUiGlyph(testCp);
        break;
      case 2:
        inFont = CjkUiFont24::hasCjkUiGlyph(testCp);
        break;
      }
      if (inFont) {
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
      const char *ptr = text;
      const EpdFontFamily &fontFamily = fontMap.at(fontId);
      FontManager &fm = FontManager::getInstance();
      uint32_t cp;
      while (
          (cp = utf8NextCodepoint(reinterpret_cast<const uint8_t **>(&ptr)))) {
        // Check if character is in our UI font (includes CJK and English)
        uint8_t actualWidth = 0;
        switch (uiFontSize) {
        case 0:
          actualWidth = CjkUiFont20::getCjkUiGlyphWidth(cp);
          break;
        case 1:
          actualWidth = CjkUiFont22::getCjkUiGlyphWidth(cp);
          break;
        case 2:
          actualWidth = CjkUiFont24::getCjkUiGlyphWidth(cp);
          break;
        }

        if (actualWidth > 0) {
          // Character is in UI font: use actual proportional width
          width += actualWidth;
        } else if (isCjkCodepoint(cp)) {
          // CJK character not in UI font: try UI external font, then reader
          ExternalFont *uiExtFont = nullptr;
          if (fm.isUiFontEnabled()) {
            uiExtFont = fm.getActiveUiFont();
          } else if (fm.isExternalFontEnabled()) {
            uiExtFont = fm.getActiveFont();
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
            const EpdGlyph *glyph = fontFamily.getGlyph(cp, style);
            if (glyph) {
              width += glyph->advanceX;
            }
          }
        } else {
          // Character not in UI font: use built-in font width
          const EpdGlyph *glyph = fontFamily.getGlyph(cp, style);
          if (glyph) {
            width += glyph->advanceX;
          }
        }
      }
      return width;
    }
  }

  int w = 0, h = 0;
  fontMap.at(fontId).getTextDimensions(text, &w, &h, style);
  return w;
}

void GfxRenderer::drawCenteredText(const int fontId, const int y,
                                   const char *text, const bool black,
                                   const EpdFontFamily::Style style) const {
  const int x = (getScreenWidth() - getTextWidth(fontId, text, style)) / 2;
  drawText(fontId, x, y, text, black, style);
}

void GfxRenderer::drawText(const int fontId, const int x, const int y,
                           const char *text, const bool black,
                           const EpdFontFamily::Style style) const {
  const int yPos = y + getFontAscenderSize(fontId);
  int xpos = x;

  // cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  FontManager &fm = FontManager::getInstance();

  // Check if this is a reader font that should use external font
  if (isReaderFont(fontId) && fm.isExternalFontEnabled()) {
    ExternalFont *extFont = fm.getActiveFont();
    if (extFont) {
      // Render using external font directly
      uint32_t cp;
      while (
          (cp = utf8NextCodepoint(reinterpret_cast<const uint8_t **>(&text)))) {
        const uint8_t *bitmap = extFont->getGlyph(cp);
        if (bitmap) {
          int advance = extFont->getCharWidth();
          if (isCjkCodepoint(cp)) {
            advance = clampExternalAdvance(advance, cjkSpacing);
          } else if (isAsciiDigit(cp)) {
            advance = clampExternalAdvance(advance, asciiDigitSpacing);
          } else if (isAsciiLetter(cp)) {
            advance = clampExternalAdvance(advance, asciiLetterSpacing);
          }
          renderExternalGlyph(bitmap, extFont, &xpos, yPos, black, advance);
        }
      }
      return;
    }
  }

  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return;
  }
  const auto font = fontMap.at(fontId);

  // no printable characters
  if (!font.hasPrintableChars(text, style)) {
    if (isReaderFont(fontId)) {
      if (!fm.isExternalFontEnabled()) {
        return;
      }
    } else {
      const bool hasUiGlyph = hasUiGlyphForText(text, uiFontSize);
      if (!hasUiGlyph && !fm.isUiFontEnabled() && !fm.isExternalFontEnabled()) {
        return;
      }
    }
  }

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t **>(&text)))) {
    renderChar(fontId, font, cp, &xpos, &yPos, black, style);
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2,
                           const bool state) const {
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
    // TODO: Implement
    Serial.printf("[%lu] [GFX] Line drawing not supported\n", millis());
  }
}

void GfxRenderer::drawRect(const int x, const int y, const int width,
                           const int height, const bool state) const {
  drawLine(x, y, x + width - 1, y, state);
  drawLine(x + width - 1, y, x + width - 1, y + height - 1, state);
  drawLine(x + width - 1, y + height - 1, x, y + height - 1, state);
  drawLine(x, y, x, y + height - 1, state);
}

void GfxRenderer::fillRect(const int x, const int y, const int width,
                           const int height, const bool state) const {
  for (int fillY = y; fillY < y + height; fillY++) {
    drawLine(x, fillY, x + width - 1, fillY, state);
  }
}

void GfxRenderer::drawImage(const uint8_t bitmap[], const int x, const int y,
                            const int width, const int height) const {
  // TODO: Rotate bits
  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(x, y, &rotatedX, &rotatedY);
  einkDisplay.drawImage(bitmap, rotatedX, rotatedY, width, height);
}

void GfxRenderer::drawBitmap(const Bitmap &bitmap, const int x, const int y,
                             const int maxWidth, const int maxHeight,
                             const float cropX, const float cropY) const {
  // For 1-bit bitmaps, use optimized 1-bit rendering path (no crop support for
  // 1-bit)
  if (bitmap.is1Bit() && cropX == 0.0f && cropY == 0.0f) {
    drawBitmap1Bit(bitmap, x, y, maxWidth, maxHeight);
    return;
  }

  // Cover art and images should keep their original colors in dark mode
  skipDarkModeForImages = true;
  auto cleanup = [this]() { skipDarkModeForImages = false; };

  // If in dark mode and drawing a positive image, we need a white background
  // first, as the global background is black.
  if (darkMode && renderMode == BW) {
    // skipDarkModeForImages is true, so drawPixel(..., false) will not invert
    // and will set the bit (White).
    fillRect(x, y, maxWidth, maxHeight, false);
  }

  float scale = 1.0f;
  bool isScaled = false;
  int cropPixX = std::floor(bitmap.getWidth() * cropX / 2.0f);
  int cropPixY = std::floor(bitmap.getHeight() * cropY / 2.0f);
  Serial.printf("[%lu] [GFX] Cropping %dx%d by %dx%d pix, is %s\n", millis(),
                bitmap.getWidth(), bitmap.getHeight(), cropPixX, cropPixY,
                bitmap.isTopDown() ? "top-down" : "bottom-up");

  if (maxWidth > 0 && (1.0f - cropX) * bitmap.getWidth() > maxWidth) {
    scale = static_cast<float>(maxWidth) /
            static_cast<float>((1.0f - cropX) * bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && (1.0f - cropY) * bitmap.getHeight() > maxHeight) {
    scale = std::min(
        scale, static_cast<float>(maxHeight) /
                   static_cast<float>((1.0f - cropY) * bitmap.getHeight()));
    isScaled = true;
  }
  Serial.printf("[%lu] [GFX] Scaling by %f - %s\n", millis(), scale,
                isScaled ? "scaled" : "not scaled");

  // Calculate output row size (2 bits per pixel, packed into bytes)
  // IMPORTANT: Use int, not uint8_t, to avoid overflow for images > 1020 pixels
  // wide
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto *outputRow = static_cast<uint8_t *>(malloc(outputRowSize));
  auto *rowBytes = static_cast<uint8_t *>(malloc(bitmap.getRowBytes()));

  if (!outputRow || !rowBytes) {
    Serial.printf("[%lu] [GFX] !! Failed to allocate BMP row buffers\n",
                  millis());
    free(outputRow);
    free(rowBytes);
    return;
  }

  for (int bmpY = 0; bmpY < (bitmap.getHeight() - cropPixY); bmpY++) {
    // The BMP's (0, 0) is the bottom-left corner (if the height is positive,
    // top-left if negative). Screen's (0, 0) is the top-left corner.
    int screenY =
        -cropPixY + (bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY);
    if (isScaled) {
      screenY = std::floor(screenY * scale);
    }
    screenY += y; // the offset should not be scaled
    if (screenY >= getScreenHeight()) {
      break;
    }
    if (screenY < 0) {
      continue;
    }

    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      Serial.printf("[%lu] [GFX] Failed to read row %d from bitmap\n", millis(),
                    bmpY);
      free(outputRow);
      free(rowBytes);
      return;
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
      screenX += x; // the offset should not be scaled
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      if (renderMode == BW && val < 3) {
        drawPixel(screenX, screenY);
      } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
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

void GfxRenderer::drawBitmap1Bit(const Bitmap &bitmap, const int x, const int y,
                                 const int maxWidth,
                                 const int maxHeight) const {
  // Cover art and images should keep their original colors in dark mode
  skipDarkModeForImages = true;
  auto cleanup = [this]() { skipDarkModeForImages = false; };

  // If in dark mode and drawing a positive image, we need a white background
  // first, as the global background is black.
  if (darkMode && renderMode == BW) {
    // skipDarkModeForImages is true, so drawPixel(..., false) will not invert
    // and will set the bit (White).
    fillRect(x, y, maxWidth, maxHeight, false);
  }

  float scale = 1.0f;
  bool isScaled = false;
  if (maxWidth > 0 && bitmap.getWidth() > maxWidth) {
    scale =
        static_cast<float>(maxWidth) / static_cast<float>(bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && bitmap.getHeight() > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) /
                                static_cast<float>(bitmap.getHeight()));
    isScaled = true;
  }

  // For 1-bit BMP, output is still 2-bit packed (for consistency with
  // readNextRow)
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto *outputRow = static_cast<uint8_t *>(malloc(outputRowSize));
  auto *rowBytes = static_cast<uint8_t *>(malloc(bitmap.getRowBytes()));

  if (!outputRow || !rowBytes) {
    Serial.printf("[%lu] [GFX] !! Failed to allocate 1-bit BMP row buffers\n",
                  millis());
    free(outputRow);
    free(rowBytes);
    return;
  }

  for (int bmpY = 0; bmpY < bitmap.getHeight(); bmpY++) {
    // Read rows sequentially using readNextRow
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      Serial.printf("[%lu] [GFX] Failed to read row %d from 1-bit bitmap\n",
                    millis(), bmpY);
      free(outputRow);
      free(rowBytes);
      return;
    }

    // Calculate screen Y based on whether BMP is top-down or bottom-up
    const int bmpYOffset =
        bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY;
    int screenY =
        y + (isScaled ? static_cast<int>(std::floor(bmpYOffset * scale))
                      : bmpYOffset);
    if (screenY >= getScreenHeight()) {
      continue; // Continue reading to keep row counter in sync
    }
    if (screenY < 0) {
      continue;
    }

    for (int bmpX = 0; bmpX < bitmap.getWidth(); bmpX++) {
      int screenX =
          x + (isScaled ? static_cast<int>(std::floor(bmpX * scale)) : bmpX);
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

void GfxRenderer::fillPolygon(const int *xPoints, const int *yPoints,
                              int numPoints, bool state) const {
  if (numPoints < 3)
    return;

  // Find bounding box
  int minY = yPoints[0], maxY = yPoints[0];
  for (int i = 1; i < numPoints; i++) {
    if (yPoints[i] < minY)
      minY = yPoints[i];
    if (yPoints[i] > maxY)
      maxY = yPoints[i];
  }

  // Clip to screen
  if (minY < 0)
    minY = 0;
  if (maxY >= getScreenHeight())
    maxY = getScreenHeight() - 1;

  // Allocate node buffer for scanline algorithm
  auto *nodeX = static_cast<int *>(malloc(numPoints * sizeof(int)));
  if (!nodeX) {
    Serial.printf("[%lu] [GFX] !! Failed to allocate polygon node buffer\n",
                  millis());
    return;
  }

  // Scanline fill algorithm
  for (int scanY = minY; scanY <= maxY; scanY++) {
    int nodes = 0;

    // Find all intersection points with edges
    int j = numPoints - 1;
    for (int i = 0; i < numPoints; i++) {
      if ((yPoints[i] < scanY && yPoints[j] >= scanY) ||
          (yPoints[j] < scanY && yPoints[i] >= scanY)) {
        // Calculate X intersection using fixed-point to avoid float
        int dy = yPoints[j] - yPoints[i];
        if (dy != 0) {
          nodeX[nodes++] = xPoints[i] + (scanY - yPoints[i]) *
                                            (xPoints[j] - xPoints[i]) / dy;
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
      if (startX < 0)
        startX = 0;
      if (endX >= getScreenWidth())
        endX = getScreenWidth() - 1;

      // Draw horizontal line
      for (int x = startX; x <= endX; x++) {
        drawPixel(x, scanY, state);
      }
    }
  }

  free(nodeX);
}

void GfxRenderer::clearScreen(const uint8_t color) const {
  // In dark mode, the default background should be black (0x00)
  // We use bitwise NOT to invert the color parameter for dark mode
  // This means clearScreen(0xFF) becomes clearScreen(0x00) in dark mode
  const uint8_t actualColor = darkMode ? ~color : color;
  einkDisplay.clearScreen(actualColor);
}

void GfxRenderer::invertScreen() const {
  uint8_t *buffer = einkDisplay.getFrameBuffer();
  if (!buffer) {
    Serial.printf("[%lu] [GFX] !! No framebuffer in invertScreen\n", millis());
    return;
  }
  for (int i = 0; i < EInkDisplay::BUFFER_SIZE; i++) {
    buffer[i] = ~buffer[i];
  }
}

void GfxRenderer::displayBuffer(
    const EInkDisplay::RefreshMode refreshMode) const {
  if (darkMode && refreshMode == EInkDisplay::FAST_REFRESH) {
    // Ensure differential refresh keeps black background solid in dark mode.
    einkDisplay.forceRedRamInverted();
  }
  einkDisplay.displayBuffer(refreshMode);
}

std::string GfxRenderer::truncatedText(const int fontId, const char *text,
                                       const int maxWidth,
                                       const EpdFontFamily::Style style) const {
  std::string item = text;
  int itemWidth = getTextWidth(fontId, item.c_str(), style);
  while (itemWidth > maxWidth && item.length() > 8) {
    item.replace(item.length() - 5, 5, "...");
    itemWidth = getTextWidth(fontId, item.c_str(), style);
  }
  return item;
}

// Note: Internal driver treats screen in command orientation; this library
// exposes a logical orientation
int GfxRenderer::getScreenWidth() const {
  switch (orientation) {
  case Portrait:
  case PortraitInverted:
    // 480px wide in portrait logical coordinates
    return EInkDisplay::DISPLAY_HEIGHT;
  case LandscapeClockwise:
  case LandscapeCounterClockwise:
    // 800px wide in landscape logical coordinates
    return EInkDisplay::DISPLAY_WIDTH;
  }
  return EInkDisplay::DISPLAY_HEIGHT;
}

int GfxRenderer::getScreenHeight() const {
  switch (orientation) {
  case Portrait:
  case PortraitInverted:
    // 800px tall in portrait logical coordinates
    return EInkDisplay::DISPLAY_WIDTH;
  case LandscapeClockwise:
  case LandscapeCounterClockwise:
    // 480px tall in landscape logical coordinates
    return EInkDisplay::DISPLAY_HEIGHT;
  }
  return EInkDisplay::DISPLAY_WIDTH;
}

int GfxRenderer::getSpaceWidth(const int fontId) const {
  // Space width should ALWAYS use built-in font (space is never CJK)
  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }

  return fontMap.at(fontId).getGlyph(' ', EpdFontFamily::REGULAR)->advanceX;
}

int GfxRenderer::getFontAscenderSize(const int fontId) const {
  // Check if using external font for reader fonts
  if (isReaderFont(fontId)) {
    FontManager &fm = FontManager::getInstance();
    if (fm.isExternalFontEnabled()) {
      ExternalFont *extFont = fm.getActiveFont();
      if (extFont) {
        // For external fonts, use charHeight as ascender
        return extFont->getCharHeight();
      }
    }
  }

  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }

  return fontMap.at(fontId).getData(EpdFontFamily::REGULAR)->ascender;
}

int GfxRenderer::getLineHeight(const int fontId) const {
  // Check if using external font for reader fonts
  if (isReaderFont(fontId)) {
    FontManager &fm = FontManager::getInstance();
    if (fm.isExternalFontEnabled()) {
      ExternalFont *extFont = fm.getActiveFont();
      if (extFont) {
        // Use charHeight + small spacing as line height
        return extFont->getCharHeight() + 4;
      }
    }
  }

  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }

  return fontMap.at(fontId).getData(EpdFontFamily::REGULAR)->advanceY;
}

void GfxRenderer::drawButtonHints(const int fontId, const char *btn1,
                                  const char *btn2, const char *btn3,
                                  const char *btn4) {
  const Orientation orientation = getOrientation();
  const int pageHeight = getScreenHeight();
  const int pageWidth = getScreenWidth();
  constexpr int buttonWidth = BUTTON_HINT_WIDTH;
  constexpr int buttonHeight = BUTTON_HINT_HEIGHT;
  constexpr int buttonY = BUTTON_HINT_BOTTOM_INSET; // Distance from bottom
  constexpr int textYOffset = BUTTON_HINT_TEXT_OFFSET;
  constexpr int buttonPositions[] = {25, 130, 245, 350};
  const char *labels[] = {btn1, btn2, btn3, btn4};
  const bool isLandscape = orientation == Orientation::LandscapeClockwise ||
                           orientation == Orientation::LandscapeCounterClockwise;
  const bool placeAtTop = orientation == Orientation::PortraitInverted;
  const int buttonTop = placeAtTop ? 0 : pageHeight - buttonY;

  if (isLandscape) {
    const bool placeLeft = orientation == Orientation::LandscapeClockwise;
    const int buttonLeft = placeLeft ? 0 : pageWidth - buttonWidth;
    if (orientation == Orientation::LandscapeCounterClockwise) {
      const char *tmp = labels[0];
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

void GfxRenderer::drawSideButtonHints(const int fontId, const char *topBtn,
                                      const char *bottomBtn) const {
  const Orientation orientation = getOrientation();
  const int screenWidth = getScreenWidth();
  constexpr int buttonWidth = 40;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 80; // Height on screen (width when rotated)
  constexpr int buttonX = 5;       // Distance from right edge
  // Position for the button group - buttons share a border so they're adjacent
  constexpr int topButtonY = 345; // Top button position

  const char *labels[] = {topBtn, bottomBtn};

  // Draw the shared border for both buttons as one unit
  const bool placeLeft = orientation == Orientation::PortraitInverted;
  const int x = placeLeft ? buttonX : screenWidth - buttonX - buttonWidth;
  const int y = topButtonY;

  // Draw top button outline (3 sides, bottom open)
  if (topBtn != nullptr && topBtn[0] != '\0') {
    drawLine(x, y, x + buttonWidth - 1, y);  // Top
    drawLine(x, y, x, y + buttonHeight - 1); // Left
    drawLine(x + buttonWidth - 1, y, x + buttonWidth - 1,
             y + buttonHeight - 1); // Right
  }

  // Draw shared middle border
  if ((topBtn != nullptr && topBtn[0] != '\0') ||
      (bottomBtn != nullptr && bottomBtn[0] != '\0')) {
    drawLine(x, y + buttonHeight, x + buttonWidth - 1,
             y + buttonHeight); // Shared border
  }

  // Draw bottom button outline (3 sides, top is shared)
  if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
    drawLine(x, y + buttonHeight, x,
             y + 2 * buttonHeight - 1); // Left
    drawLine(x + buttonWidth - 1, y + buttonHeight,
             x + buttonWidth - 1,
             y + 2 * buttonHeight - 1); // Right
    drawLine(x, y + 2 * buttonHeight - 1, x + buttonWidth - 1,
             y + 2 * buttonHeight - 1); // Bottom
  }

  // Draw text for each button
  for (int i = 0; i < 2; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int yPos = y + i * buttonHeight;
      const char *label = labels[i];

      // Draw rotated text centered in the button
      const int textWidth = getTextWidth(fontId, label);
      int textHeight = getTextHeight(fontId);
      bool hasCjk = false;
      const char *scan = label;
      uint32_t cp;
      while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t **>(&scan)))) {
        if (isCjkCodepoint(cp)) {
          hasCjk = true;
          break;
        }
      }
      if (hasCjk) {
        switch (uiFontSize) {
        case 0:
          textHeight = CjkUiFont20::CJK_UI_FONT_HEIGHT;
          break;
        case 1:
          textHeight = CjkUiFont22::CJK_UI_FONT_HEIGHT;
          break;
        case 2:
        default:
          textHeight = CjkUiFont24::CJK_UI_FONT_HEIGHT;
          break;
        }
      }

      // Center the rotated text in the button
      const int textX = x + (buttonWidth - textHeight) / 2;
      const int textY = yPos + (buttonHeight + textWidth) / 2;

      drawTextRotated90CW(fontId, textX, textY, label);
    }
  }
}

int GfxRenderer::getTextHeight(const int fontId) const {
  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }
  return fontMap.at(fontId).getData(EpdFontFamily::REGULAR)->ascender;
}

void GfxRenderer::drawTextRotated90CW(const int fontId, const int x,
                                      const int y, const char *text,
                                      const bool black,
                                      const EpdFontFamily::Style style) const {
  // Cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return;
  }
  const auto font = fontMap.at(fontId);

  // No printable characters
  if (!font.hasPrintableChars(text, style)) {
    if (isReaderFont(fontId)) {
      return;
    }
    if (!hasUiGlyphForText(text, uiFontSize)) {
      return;
    }
  }

  // For 90° clockwise rotation:
  // Original (glyphX, glyphY) -> Rotated (glyphY, -glyphX)
  // Text reads from bottom to top

  int yPos = y; // Current Y position (decreases as we draw characters)

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t **>(&text)))) {
    // For ASCII characters, prefer EPD font (better quality for Latin text)
    // Only use CJK UI font for non-ASCII characters or when EPD font lacks the glyph
    bool useEpdFont = (cp < 0x80) && font.getGlyph(cp, style) != nullptr;

    if (!isReaderFont(fontId) && !useEpdFont) {
      const uint8_t *bitmap = nullptr;
      uint8_t fontWidth = 0;
      uint8_t fontHeight = 0;
      uint8_t bytesPerRow = 0;
      uint8_t bytesPerChar = 0;
      uint8_t advance = 0;

      switch (uiFontSize) {
      case 0: // SMALL - 20x20
        if (CjkUiFont20::hasCjkUiGlyph(cp)) {
          bitmap = CjkUiFont20::getCjkUiGlyph(cp);
          fontWidth = CjkUiFont20::CJK_UI_FONT_WIDTH;
          fontHeight = CjkUiFont20::CJK_UI_FONT_HEIGHT;
          bytesPerRow = CjkUiFont20::CJK_UI_FONT_BYTES_PER_ROW;
          bytesPerChar = CjkUiFont20::CJK_UI_FONT_BYTES_PER_CHAR;
          advance = CjkUiFont20::getCjkUiGlyphWidth(cp);
        }
        break;
      case 1: // MEDIUM - 22x22
        if (CjkUiFont22::hasCjkUiGlyph(cp)) {
          bitmap = CjkUiFont22::getCjkUiGlyph(cp);
          fontWidth = CjkUiFont22::CJK_UI_FONT_WIDTH;
          fontHeight = CjkUiFont22::CJK_UI_FONT_HEIGHT;
          bytesPerRow = CjkUiFont22::CJK_UI_FONT_BYTES_PER_ROW;
          bytesPerChar = CjkUiFont22::CJK_UI_FONT_BYTES_PER_CHAR;
          advance = CjkUiFont22::getCjkUiGlyphWidth(cp);
        }
        break;
      case 2: // LARGE - 24x24
      default:
        if (CjkUiFont24::hasCjkUiGlyph(cp)) {
          bitmap = CjkUiFont24::getCjkUiGlyph(cp);
          fontWidth = CjkUiFont24::CJK_UI_FONT_WIDTH;
          fontHeight = CjkUiFont24::CJK_UI_FONT_HEIGHT;
          bytesPerRow = CjkUiFont24::CJK_UI_FONT_BYTES_PER_ROW;
          bytesPerChar = CjkUiFont24::CJK_UI_FONT_BYTES_PER_CHAR;
          advance = CjkUiFont24::getCjkUiGlyphWidth(cp);
        }
        break;
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

    const EpdGlyph *glyph = font.getGlyph(cp, style);
    if (!glyph) {
      glyph = font.getGlyph('?', style);
    }
    if (!glyph) {
      continue;
    }

    const int is2Bit = font.getData(style)->is2Bit;
    const uint32_t offset = glyph->dataOffset;
    const uint8_t width = glyph->width;
    const uint8_t height = glyph->height;
    const int left = glyph->left;
    const int top = glyph->top;

    const uint8_t *bitmap = &font.getData(style)->bitmap[offset];

    if (bitmap != nullptr) {
      for (int glyphY = 0; glyphY < height; glyphY++) {
        for (int glyphX = 0; glyphX < width; glyphX++) {
          const int pixelPosition = glyphY * width + glyphX;

          // 90° clockwise rotation transformation:
          // screenX = x + (ascender - top + glyphY)
          // screenY = yPos - (left + glyphX)
          const int screenX =
              x + (font.getData(style)->ascender - top + glyphY);
          const int screenY = yPos - left - glyphX;

          if (is2Bit) {
            const uint8_t byte = bitmap[pixelPosition / 4];
            const uint8_t bit_index = (3 - pixelPosition % 4) * 2;
            const uint8_t bmpVal = 3 - (byte >> bit_index) & 0x3;

            if (renderMode == BW && bmpVal < 3) {
              drawPixel(screenX, screenY, black);
            } else if (renderMode == GRAYSCALE_MSB &&
                       (bmpVal == 1 || bmpVal == 2)) {
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

uint8_t *GfxRenderer::getFrameBuffer() const {
  return einkDisplay.getFrameBuffer();
}

size_t GfxRenderer::getBufferSize() { return EInkDisplay::BUFFER_SIZE; }

void GfxRenderer::grayscaleRevert() const { einkDisplay.grayscaleRevert(); }

void GfxRenderer::copyGrayscaleLsbBuffers() const {
  einkDisplay.copyGrayscaleLsbBuffers(einkDisplay.getFrameBuffer());
}

void GfxRenderer::copyGrayscaleMsbBuffers() const {
  einkDisplay.copyGrayscaleMsbBuffers(einkDisplay.getFrameBuffer());
}

void GfxRenderer::displayGrayBuffer(const bool turnOffScreen,
                                    const bool darkMode) const {
  einkDisplay.displayGrayBuffer(turnOffScreen, darkMode);
}

void GfxRenderer::freeBwBufferChunks() {
  for (auto &bwBufferChunk : bwBufferChunks) {
    if (bwBufferChunk) {
      free(bwBufferChunk);
      bwBufferChunk = nullptr;
    }
  }
}

/**
 * This should be called before grayscale buffers are populated.
 * A `restoreBwBuffer` call should always follow the grayscale render if this
 * method was called. Uses chunked allocation to avoid needing 48KB of
 * contiguous memory. Returns true if buffer was stored successfully, false if
 * allocation failed.
 */
bool GfxRenderer::storeBwBuffer() {
  const uint8_t *frameBuffer = einkDisplay.getFrameBuffer();
  if (!frameBuffer) {
    Serial.printf("[%lu] [GFX] !! No framebuffer in storeBwBuffer\n", millis());
    return false;
  }

  // Allocate and copy each chunk
  for (size_t i = 0; i < BW_BUFFER_NUM_CHUNKS; i++) {
    // Check if any chunks are already allocated
    if (bwBufferChunks[i]) {
      Serial.printf("[%lu] [GFX] !! BW buffer chunk %zu already stored - this "
                    "is likely a bug, freeing chunk\n",
                    millis(), i);
      free(bwBufferChunks[i]);
      bwBufferChunks[i] = nullptr;
    }

    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    bwBufferChunks[i] = static_cast<uint8_t *>(malloc(BW_BUFFER_CHUNK_SIZE));

    if (!bwBufferChunks[i]) {
      Serial.printf(
          "[%lu] [GFX] !! Failed to allocate BW buffer chunk %zu (%zu bytes)\n",
          millis(), i, BW_BUFFER_CHUNK_SIZE);
      // Free previously allocated chunks
      freeBwBufferChunks();
      return false;
    }

    memcpy(bwBufferChunks[i], frameBuffer + offset, BW_BUFFER_CHUNK_SIZE);
  }

  Serial.printf("[%lu] [GFX] Stored BW buffer in %zu chunks (%zu bytes each)\n",
                millis(), BW_BUFFER_NUM_CHUNKS, BW_BUFFER_CHUNK_SIZE);
  return true;
}

/**
 * This can only be called if `storeBwBuffer` was called prior to the grayscale
 * render. It should be called to restore the BW buffer state after grayscale
 * rendering is complete. Uses chunked restoration to match chunked storage.
 */
void GfxRenderer::restoreBwBuffer() {
  // Check if any all chunks are allocated
  bool missingChunks = false;
  for (const auto &bwBufferChunk : bwBufferChunks) {
    if (!bwBufferChunk) {
      missingChunks = true;
      break;
    }
  }

  if (missingChunks) {
    freeBwBufferChunks();
    return;
  }

  uint8_t *frameBuffer = einkDisplay.getFrameBuffer();
  if (!frameBuffer) {
    Serial.printf("[%lu] [GFX] !! No framebuffer in restoreBwBuffer\n",
                  millis());
    freeBwBufferChunks();
    return;
  }

  for (size_t i = 0; i < BW_BUFFER_NUM_CHUNKS; i++) {
    // Check if chunk is missing
    if (!bwBufferChunks[i]) {
      Serial.printf(
          "[%lu] [GFX] !! BW buffer chunks not stored - this is likely a bug\n",
          millis());
      freeBwBufferChunks();
      return;
    }

    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    memcpy(frameBuffer + offset, bwBufferChunks[i], BW_BUFFER_CHUNK_SIZE);
  }

  einkDisplay.cleanupGrayscaleBuffers(frameBuffer);

  freeBwBufferChunks();
  Serial.printf("[%lu] [GFX] Restored and freed BW buffer chunks\n", millis());
}

/**
 * Cleanup grayscale buffers using the current frame buffer.
 * Use this when BW buffer was re-rendered instead of stored/restored.
 */
void GfxRenderer::cleanupGrayscaleWithFrameBuffer() const {
  uint8_t *frameBuffer = einkDisplay.getFrameBuffer();
  if (frameBuffer) {
    einkDisplay.cleanupGrayscaleBuffers(frameBuffer);
  }
}

// Check if fontId is a reader font (should use external Chinese font)
// UI fonts (UI_10, UI_12, SMALL_FONT) should NOT use external font
bool GfxRenderer::isReaderFont(const int fontId) {
  // First check if it's a UI font - UI fonts should NOT use external reader font
  for (int i = 0; i < UI_FONT_COUNT; i++) {
    if (UI_FONT_IDS[i] == fontId) {
      return false; // This is a UI font, not a reader font
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

void GfxRenderer::renderChar(const int fontId, const EpdFontFamily &fontFamily,
                             const uint32_t cp, int *x, const int *y,
                             const bool pixelState,
                             const EpdFontFamily::Style style) const {
  FontManager &fm = FontManager::getInstance();

  // Try external font for CJK + ASCII letters/digits
  // Reader fonts: use reader external font
  // UI fonts: use UI external font (with fallback to reader font)
  if (isReaderFont(fontId)) {
    // Reader font - use reader external font for CJK + ASCII letters/digits.
    if (fm.isExternalFontEnabled()) {
      ExternalFont *extFont = fm.getActiveFont();
      if (extFont) {
        const bool useExternal =
            isCjkCodepoint(cp) || isAsciiLetter(cp) || isAsciiDigit(cp);
        if (useExternal) {
          const uint8_t *bitmap = extFont->getGlyph(cp);
          if (bitmap) {
            int advance = extFont->getCharWidth();
            if (isCjkCodepoint(cp)) {
              advance = clampExternalAdvance(advance, cjkSpacing);
            } else if (isAsciiDigit(cp)) {
              advance = clampExternalAdvance(advance, asciiDigitSpacing);
            } else if (isAsciiLetter(cp)) {
              advance = clampExternalAdvance(advance, asciiLetterSpacing);
            }
            renderExternalGlyph(bitmap, extFont, x, *y, pixelState, advance);
            return;
          }
          // Glyph not found in external font, fall through to built-in font
        }
      }
    }
  } else {
    // UI font - for CJK characters, prioritize external UI font over built-in
    if (isCjkCodepoint(cp)) {
      // Debug: log UI font status (only once per session)
      static bool debugLogged = false;
      if (!debugLogged) {
        Serial.printf("[GFX] UI font debug: isUiFontEnabled=%d, isExternalFontEnabled=%d\n",
                      fm.isUiFontEnabled(), fm.isExternalFontEnabled());
        ExternalFont *uiFont = fm.getActiveUiFont();
        Serial.printf("[GFX] UI font ptr=%p, loaded=%d\n", uiFont,
                      uiFont ? uiFont->isLoaded() : -1);
        debugLogged = true;
      }

      // First try external UI font (user's choice)
      if (fm.isUiFontEnabled()) {
        ExternalFont *uiExtFont = fm.getActiveUiFont();
        if (uiExtFont) {
          const uint8_t *bitmap = uiExtFont->getGlyph(cp);
          if (bitmap) {
            // Get actual advance width for proportional spacing
            uint8_t advanceX = 0;
            uiExtFont->getGlyphMetrics(cp, nullptr, &advanceX);
            renderExternalGlyph(bitmap, uiExtFont, x, *y, pixelState, advanceX);
            return;
          }
        }
      }

      // Then check built-in CJK UI font (when UI font is disabled or glyph not found)
      bool hasGlyph = false;
      switch (uiFontSize) {
      case 0: // SMALL - 20x20
        hasGlyph = CjkUiFont20::hasCjkUiGlyph(cp);
        break;
      case 1: // MEDIUM - 22x22
        hasGlyph = CjkUiFont22::hasCjkUiGlyph(cp);
        break;
      case 2: // LARGE - 24x24
      default:
        hasGlyph = CjkUiFont24::hasCjkUiGlyph(cp);
        break;
      }

      if (hasGlyph) {
        renderBuiltinCjkGlyph(cp, x, *y, pixelState);
        return;
      }

      // Last resort: try reader font if built-in doesn't have this glyph
      if (fm.isExternalFontEnabled()) {
        ExternalFont *extFont = fm.getActiveFont();
        if (extFont) {
          const uint8_t *bitmap = extFont->getGlyph(cp);
          if (bitmap) {
            // Get actual advance width for proportional spacing
            uint8_t advanceX = 0;
            extFont->getGlyphMetrics(cp, nullptr, &advanceX);
            renderExternalGlyph(bitmap, extFont, x, *y, pixelState, advanceX);
            return;
          }
        }
      }
    } else {
      // Non-CJK characters in UI - if external UI font is enabled, try it first
      // This ensures consistent font rendering when user selects an external UI font
      if (fm.isUiFontEnabled()) {
        ExternalFont *uiExtFont = fm.getActiveUiFont();
        if (uiExtFont) {
          const uint8_t *bitmap = uiExtFont->getGlyph(cp);
          if (bitmap) {
            // Get actual advance width for proportional spacing
            uint8_t advanceX = 0;
            uiExtFont->getGlyphMetrics(cp, nullptr, &advanceX);
            renderExternalGlyph(bitmap, uiExtFont, x, *y, pixelState, advanceX);
            return;
          }
        }
      }
      // Fall through to EPD font rendering below
    }
  }

  const EpdGlyph *glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) {
    // TODO: Replace with fallback glyph property?
    glyph = fontFamily.getGlyph('?', style);
  }

  // no glyph?
  if (!glyph) {
    Serial.printf("[%lu] [GFX] No glyph for codepoint %d\n", millis(), cp);
    return;
  }

  const int is2Bit = fontFamily.getData(style)->is2Bit;
  const uint32_t offset = glyph->dataOffset;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int left = glyph->left;

  const uint8_t *bitmap = nullptr;
  bitmap = &fontFamily.getData(style)->bitmap[offset];

  if (bitmap != nullptr) {
    for (int glyphY = 0; glyphY < height; glyphY++) {
      const int screenY = *y - glyph->top + glyphY;
      for (int glyphX = 0; glyphX < width; glyphX++) {
        const int pixelPosition = glyphY * width + glyphX;
        const int screenX = *x + left + glyphX;

        if (is2Bit) {
          const uint8_t byte = bitmap[pixelPosition / 4];
          const uint8_t bit_index = (3 - pixelPosition % 4) * 2;
          // the direct bit from the font is 0 -> white, 1 -> light gray, 2 ->
          // dark gray, 3 -> black we swap this to better match the way images
          // and screen think about colors: 0 -> black, 1 -> dark grey, 2 ->
          // light grey, 3 -> white
          const uint8_t bmpVal = 3 - (byte >> bit_index) & 0x3;

          if (renderMode == BW) {
            bool shouldDraw = false;
            if (darkMode) {
              // In dark mode, do NOT draw anti-aliasing edges (bmpVal 1, 2)
              // as they must remain black (background) to be lightened to gray
              // in the grayscale pass. Only draw core ink (bmpVal 0).
              if (bmpVal == 0) {
                shouldDraw = true;
              }
            } else if (bmpVal < 3) {
              // In light mode, draw core and grays as black.
              // They will be lightened to grays later.
              shouldDraw = true;
            }

            if (shouldDraw) {
              drawPixel(screenX, screenY, pixelState);
            }
          } else if (renderMode == GRAYSCALE_MSB ||
                     renderMode == GRAYSCALE_LSB) {
            // Processing for grayscale buffers (LSB/MSB)
            // Map pixel values:
            // Light Mode: 0=Black(00), 1=DarkGray(01), 2=LightGray(10),
            // 3=White(11) Dark Mode:  Invert brightness -> 0=White(11),
            // 1=LightGray(10), 2=DarkGray(01), 3=Black(00)

            // Only draw ink pixels (val < 3). Assume background (val 3) is
            // already cleared correctly.
            if (bmpVal < 3) {
              uint8_t val = bmpVal;
              if (darkMode) {
                // Invert brightness for dark mode
                val = 3 - val;
              }

              // Extract bit for current plane
              bool bit =
                  (renderMode == GRAYSCALE_LSB) ? (val & 1) : ((val >> 1) & 1);

              // drawPixel(true) -> Clear bit (0), drawPixel(false) -> Set bit
              // (1)
              drawPixel(screenX, screenY, !bit);
            } else if (darkMode && bmpVal == 3) {
              // For Dark Mode Background (bmpVal=3, White in source), we want
              // it to be mapped to 11 (Group 3) The new LUT will treat Group 3
              // as "Drive Black". Existing buffer clear color for Dark Mode is
              // 0xFF (11). So we don't need to do anything - the background is
              // already 11.
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

  *x += glyph->advanceX;
}

void GfxRenderer::getOrientedViewableTRBL(int *outTop, int *outRight,
                                          int *outBottom, int *outLeft) const {
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

void GfxRenderer::renderExternalGlyph(const uint8_t *bitmap, ExternalFont *font,
                                      int *x, const int y,
                                      const bool pixelState,
                                      const int advanceOverride) const {
  const uint8_t width = font->getCharWidth();
  const uint8_t height = font->getCharHeight();
  const uint8_t bytesPerRow = font->getBytesPerRow();

  // Calculate starting Y position (baseline alignment)
  const int startY = y - height + 4; // +4 is baseline adjustment

  for (int glyphY = 0; glyphY < height; glyphY++) {
    const int screenY = startY + glyphY;
    for (int glyphX = 0; glyphX < width; glyphX++) {
      const int byteIndex = glyphY * bytesPerRow + (glyphX / 8);
      const int bitIndex = 7 - (glyphX % 8); // MSB first

      if ((bitmap[byteIndex] >> bitIndex) & 1) {
        drawPixel(*x + glyphX, screenY, pixelState);
      }
    }
  }

  // Advance cursor
  const int advance = (advanceOverride >= 0) ? advanceOverride : width;
  *x += std::max(1, advance);
}

void GfxRenderer::renderBuiltinCjkGlyph(const uint32_t cp, int *x, const int y,
                                        const bool pixelState) const {
  // Select font based on uiFontSize setting
  const uint8_t *bitmap = nullptr;
  uint8_t fontWidth = 0;
  uint8_t fontHeight = 0;
  uint8_t bytesPerRow = 0;
  uint8_t bytesPerChar = 0;
  uint8_t actualWidth = 0; // Actual advance width for proportional spacing

  switch (uiFontSize) {
  case 0: // SMALL - 20x20
    bitmap = CjkUiFont20::getCjkUiGlyph(cp);
    fontWidth = CjkUiFont20::CJK_UI_FONT_WIDTH;
    fontHeight = CjkUiFont20::CJK_UI_FONT_HEIGHT;
    bytesPerRow = CjkUiFont20::CJK_UI_FONT_BYTES_PER_ROW;
    bytesPerChar = CjkUiFont20::CJK_UI_FONT_BYTES_PER_CHAR;
    actualWidth = CjkUiFont20::getCjkUiGlyphWidth(cp);
    break;
  case 1: // MEDIUM - 22x22
    bitmap = CjkUiFont22::getCjkUiGlyph(cp);
    fontWidth = CjkUiFont22::CJK_UI_FONT_WIDTH;
    fontHeight = CjkUiFont22::CJK_UI_FONT_HEIGHT;
    bytesPerRow = CjkUiFont22::CJK_UI_FONT_BYTES_PER_ROW;
    bytesPerChar = CjkUiFont22::CJK_UI_FONT_BYTES_PER_CHAR;
    actualWidth = CjkUiFont22::getCjkUiGlyphWidth(cp);
    break;
  case 2: // LARGE - 24x24
  default:
    bitmap = CjkUiFont24::getCjkUiGlyph(cp);
    fontWidth = CjkUiFont24::CJK_UI_FONT_WIDTH;
    fontHeight = CjkUiFont24::CJK_UI_FONT_HEIGHT;
    bytesPerRow = CjkUiFont24::CJK_UI_FONT_BYTES_PER_ROW;
    bytesPerChar = CjkUiFont24::CJK_UI_FONT_BYTES_PER_CHAR;
    actualWidth = CjkUiFont24::getCjkUiGlyphWidth(cp);
    break;
  }

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
    // Calculate starting Y position for CJK fonts
    // The 'y' parameter is the baseline position
    // For UI fonts, we need to align CJK glyphs with the baseline
    // CJK fonts are typically designed to sit on the baseline with some descent
    // Use a simpler calculation: place CJK glyph so its bottom aligns near baseline
    const int startY = y - fontHeight + 4; // 4px descent for CJK characters

    for (int glyphY = 0; glyphY < fontHeight; glyphY++) {
      const int screenY = startY + glyphY;
      for (int glyphX = 0; glyphX < fontWidth; glyphX++) {
        const int byteIndex = glyphY * bytesPerRow + (glyphX / 8);
        const int bitIndex = 7 - (glyphX % 8); // MSB first

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
