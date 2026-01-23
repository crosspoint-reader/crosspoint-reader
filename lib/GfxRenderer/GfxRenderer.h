#pragma once

#include <EInkDisplay.h>
#include <EpdFontFamily.h>

#include <map>

#include "Bitmap.h"

// Forward declaration for external font support
class ExternalFont;

class GfxRenderer {
public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

  // Logical screen orientation from the perspective of callers
  enum Orientation {
    Portrait,                 // 480x800 logical coordinates (current default)
    LandscapeClockwise,       // 800x480 logical coordinates, rotated 180° (swap
                              // top/bottom)
    PortraitInverted,         // 480x800 logical coordinates, inverted
    LandscapeCounterClockwise // 800x480 logical coordinates, native panel
                              // orientation
  };

private:
  static constexpr size_t BW_BUFFER_CHUNK_SIZE =
      8000; // 8KB chunks to allow for non-contiguous memory
  static constexpr size_t BW_BUFFER_NUM_CHUNKS =
      EInkDisplay::BUFFER_SIZE / BW_BUFFER_CHUNK_SIZE;
  static_assert(BW_BUFFER_CHUNK_SIZE * BW_BUFFER_NUM_CHUNKS ==
                    EInkDisplay::BUFFER_SIZE,
                "BW buffer chunking does not line up with display buffer size");

  EInkDisplay &einkDisplay;
  RenderMode renderMode;
  Orientation orientation;
  uint8_t *bwBufferChunks[BW_BUFFER_NUM_CHUNKS] = {nullptr};
  std::map<int, EpdFontFamily> fontMap;
  // UI font size: 0=20px(SMALL), 1=22px(MEDIUM), 2=24px(LARGE)
  uint8_t uiFontSize = 0;
  // Dark mode: true = black background, false = white background
  bool darkMode = false;
  // Extra spacing (in pixels) for ASCII letters/digits when using external reader font.
  int8_t asciiLetterSpacing = 0;
  int8_t asciiDigitSpacing = 0;
  // Extra spacing (in pixels) for CJK characters when using external reader font.
  int8_t cjkSpacing = 0;
  // Skip dark mode inversion for images (cover art should not be inverted)
  mutable bool skipDarkModeForImages = false;
  void renderChar(int fontId, const EpdFontFamily &fontFamily, uint32_t cp,
                  int *x, const int *y, bool pixelState,
                  EpdFontFamily::Style style) const;
  void renderExternalGlyph(const uint8_t *bitmap, ExternalFont *font, int *x,
                           int y, bool pixelState,
                           int advanceOverride = -1) const;
  // Render CJK character using built-in UI font (from PROGMEM)
  void renderBuiltinCjkGlyph(uint32_t cp, int *x, int y, bool pixelState) const;
  // Check if fontId is a reader font (should use external Chinese font)
  static bool isReaderFont(int fontId);
  void freeBwBufferChunks();
  void rotateCoordinates(int x, int y, int *rotatedX, int *rotatedY) const;

public:
  explicit GfxRenderer(EInkDisplay &einkDisplay)
      : einkDisplay(einkDisplay), renderMode(BW), orientation(Portrait) {}
  ~GfxRenderer() { freeBwBufferChunks(); }

  static constexpr int VIEWABLE_MARGIN_TOP = 9;
  static constexpr int VIEWABLE_MARGIN_RIGHT = 3;
  static constexpr int VIEWABLE_MARGIN_BOTTOM = 3;
  static constexpr int VIEWABLE_MARGIN_LEFT = 3;
  static constexpr int BUTTON_HINT_WIDTH = 106;
  static constexpr int BUTTON_HINT_HEIGHT = 40;
  static constexpr int BUTTON_HINT_BOTTOM_INSET = 40;
  static constexpr int BUTTON_HINT_TEXT_OFFSET = 7;

  // Setup
  void insertFont(int fontId, EpdFontFamily font);

  // Orientation control (affects logical width/height and coordinate
  // transforms)
  void setOrientation(const Orientation o) { orientation = o; }
  Orientation getOrientation() const { return orientation; }

  // UI font size control (0=20px, 1=22px, 2=24px)
  void setUiFontSize(uint8_t size) { uiFontSize = (size > 2) ? 2 : size; }
  uint8_t getUiFontSize() const { return uiFontSize; }

  // Dark mode control
  void setDarkMode(bool darkMode) { this->darkMode = darkMode; }
  bool isDarkMode() const { return darkMode; }
  void setAsciiLetterSpacing(int8_t spacing) { asciiLetterSpacing = spacing; }
  void setAsciiDigitSpacing(int8_t spacing) { asciiDigitSpacing = spacing; }
  void setCjkSpacing(int8_t spacing) { cjkSpacing = spacing; }
  int8_t getAsciiLetterSpacing() const { return asciiLetterSpacing; }
  int8_t getAsciiDigitSpacing() const { return asciiDigitSpacing; }
  int8_t getCjkSpacing() const { return cjkSpacing; }

  // Screen ops
  int getScreenWidth() const;
  int getScreenHeight() const;
  void displayBuffer(
      EInkDisplay::RefreshMode refreshMode = EInkDisplay::FAST_REFRESH) const;
  // EXPERIMENTAL: Windowed update - display only a rectangular region
  void displayWindow(int x, int y, int width, int height) const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;

  // Drawing
  void drawPixel(int x, int y, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width,
                 int height) const;
  void drawBitmap(const Bitmap &bitmap, int x, int y, int maxWidth,
                  int maxHeight, float cropX = 0, float cropY = 0) const;
  void drawBitmap1Bit(const Bitmap &bitmap, int x, int y, int maxWidth,
                      int maxHeight) const;
  void fillPolygon(const int *xPoints, const int *yPoints, int numPoints,
                   bool state = true) const;

  // Text
  int getTextWidth(int fontId, const char *text,
                   EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void
  drawCenteredText(int fontId, int y, const char *text, bool black = true,
                   EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawText(int fontId, int x, int y, const char *text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getSpaceWidth(int fontId) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
  std::string
  truncatedText(int fontId, const char *text, int maxWidth,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  // UI Components
  void drawButtonHints(int fontId, const char *btn1, const char *btn2,
                       const char *btn3, const char *btn4);
  void drawSideButtonHints(int fontId, const char *topBtn,
                           const char *bottomBtn) const;

private:
  // Helper for drawing rotated text (90 degrees clockwise, for side buttons)
  void drawTextRotated90CW(
      int fontId, int x, int y, const char *text, bool black = true,
      EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextHeight(int fontId) const;

public:
  // Grayscale functions
  void setRenderMode(const RenderMode mode) { this->renderMode = mode; }
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer(bool turnOffScreen = false,
                         bool darkMode = false) const;
  bool storeBwBuffer();   // Returns true if buffer was stored successfully
  void restoreBwBuffer(); // Restore and free the stored buffer
  void cleanupGrayscaleWithFrameBuffer() const;

  // Low level functions
  uint8_t *getFrameBuffer() const;
  static size_t getBufferSize();
  void grayscaleRevert() const;
  void getOrientedViewableTRBL(int *outTop, int *outRight, int *outBottom,
                               int *outLeft) const;
};
