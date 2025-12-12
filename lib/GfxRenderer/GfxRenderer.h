#pragma once

#include <EInkDisplay.h>
#include <EpdFontFamily.h>

#include <map>

class GfxRenderer {
 public:
  enum FontRenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

 private:
  EInkDisplay& einkDisplay;
  FontRenderMode fontRenderMode;
  std::map<int, EpdFontFamily> fontMap;
  void renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, const int* y, bool pixelState,
                  EpdFontStyle style) const;

 public:
  explicit GfxRenderer(EInkDisplay& einkDisplay) : einkDisplay(einkDisplay), fontRenderMode(BW) {}
  ~GfxRenderer() = default;

  // Setup
  void insertFont(int fontId, EpdFontFamily font);

  // Screen ops
  static int getScreenWidth();
  static int getScreenHeight();
  void displayBuffer(EInkDisplay::RefreshMode refreshMode = EInkDisplay::FAST_REFRESH) const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;

  // Drawing
  void drawPixel(int x, int y, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;

  // Text
  int getTextWidth(int fontId, const char* text, EpdFontStyle style = REGULAR) const;
  void drawCenteredText(int fontId, int y, const char* text, bool black = true, EpdFontStyle style = REGULAR) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true, EpdFontStyle style = REGULAR) const;
  void setFontRenderMode(const FontRenderMode mode) { this->fontRenderMode = mode; }
  int getSpaceWidth(int fontId) const;
  int getLineHeight(int fontId) const;

  // Low level functions
  void swapBuffers() const;
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer() const;
};
