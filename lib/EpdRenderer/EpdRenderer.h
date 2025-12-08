#pragma once

#include <EInkDisplay.h>

#include <EpdFontRenderer.hpp>

class EpdRenderer {
  EInkDisplay& einkDisplay;
  EpdFontRenderer<EInkDisplay>* regularFontRenderer;
  EpdFontRenderer<EInkDisplay>* smallFontRenderer;
  EpdFontRenderer<EInkDisplay>* uiFontRenderer;
  int marginTop;
  int marginBottom;
  int marginLeft;
  int marginRight;
  EpdFontRendererMode fontRendererMode;
  float lineCompression;

 public:
  explicit EpdRenderer(EInkDisplay& einkDisplay);
  ~EpdRenderer();
  void drawPixel(int x, int y, bool state = true) const;
  int getTextWidth(const char* text, EpdFontStyle style = REGULAR) const;
  int getUiTextWidth(const char* text, EpdFontStyle style = REGULAR) const;
  int getSmallTextWidth(const char* text, EpdFontStyle style = REGULAR) const;
  void drawText(int x, int y, const char* text, bool state = true, EpdFontStyle style = REGULAR) const;
  void drawUiText(int x, int y, const char* text, bool state = true, EpdFontStyle style = REGULAR) const;
  void drawSmallText(int x, int y, const char* text, bool state = true, EpdFontStyle style = REGULAR) const;
  void drawTextBox(int x, int y, const std::string& text, int width, int height, EpdFontStyle style = REGULAR) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawImageNoMargin(const uint8_t bitmap[], int x, int y, int width, int height) const;

  void swapBuffers() const;
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer() const;
  void clearScreen(uint8_t color = 0xFF) const;

  void invertScreen() const;

  void flushDisplay(EInkDisplay::RefreshMode refreshMode = EInkDisplay::FAST_REFRESH) const;
  // void flushArea(int x, int y, int width, int height) const;

  int getPageWidth() const;
  int getPageHeight() const;
  int getSpaceWidth() const;
  int getLineHeight() const;

  // set margins
  void setMarginTop(const int newMarginTop) { this->marginTop = newMarginTop; }
  void setMarginBottom(const int newMarginBottom) { this->marginBottom = newMarginBottom; }
  void setMarginLeft(const int newMarginLeft) { this->marginLeft = newMarginLeft; }
  void setMarginRight(const int newMarginRight) { this->marginRight = newMarginRight; }
  void setFontRendererMode(const EpdFontRendererMode mode) { this->fontRendererMode = mode; }
};
