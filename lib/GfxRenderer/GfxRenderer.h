#pragma once

#include <EInkDisplay.h>
#include <EpdFontFamily.h>
#include <FS.h>

#include <map>

#include "Bitmap.h"

class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

 private:
  static constexpr size_t BW_BUFFER_CHUNK_SIZE = 8000;  // 8KB chunks to allow for non-contiguous memory
  static constexpr size_t BW_BUFFER_NUM_CHUNKS = EInkDisplay::BUFFER_SIZE / BW_BUFFER_CHUNK_SIZE;
  static_assert(BW_BUFFER_CHUNK_SIZE * BW_BUFFER_NUM_CHUNKS == EInkDisplay::BUFFER_SIZE,
                "BW buffer chunking does not line up with display buffer size");

  EInkDisplay& einkDisplay;
  RenderMode renderMode;
  uint8_t* bwBufferChunks[BW_BUFFER_NUM_CHUNKS] = {nullptr};
  std::map<int, EpdFontFamily> fontMap;
  void renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, const int* y, bool pixelState,
                  EpdFontStyle style) const;
  void freeBwBufferChunks();

 public:
  explicit GfxRenderer(EInkDisplay& einkDisplay) : einkDisplay(einkDisplay), renderMode(BW) {}
  ~GfxRenderer() = default;

  // Setup
  void insertFont(int fontId, EpdFontFamily font);

  // Screen ops
  static int getScreenWidth();
  static int getScreenHeight();
  void displayBuffer(EInkDisplay::RefreshMode refreshMode = EInkDisplay::FAST_REFRESH) const;
  // EXPERIMENTAL: Windowed update - display only a rectangular region (portrait coordinates)
  void displayWindow(int x, int y, int width, int height) const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;

  // Drawing
  void drawPixel(int x, int y, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state = true) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const;
  void drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool state) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight) const;

  // Text
  int getTextWidth(int fontId, const char* text, EpdFontStyle style = REGULAR) const;
  void drawCenteredText(int fontId, int y, const char* text, bool black = true, EpdFontStyle style = REGULAR) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true, EpdFontStyle style = REGULAR) const;
  void drawTextInBox(int fontId, int x, int y, int w, int h, const char* text, bool centered, bool black = true, EpdFontStyle style = REGULAR) const;
  int getSpaceWidth(int fontId) const;
  int getLineHeight(int fontId) const;

  // Grayscale functions
  void setRenderMode(const RenderMode mode) { this->renderMode = mode; }
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer() const;
  void storeBwBuffer();
  void restoreBwBuffer();

  // Low level functions
  uint8_t* getFrameBuffer() const;
  static size_t getBufferSize();
  void grayscaleRevert() const;
};
