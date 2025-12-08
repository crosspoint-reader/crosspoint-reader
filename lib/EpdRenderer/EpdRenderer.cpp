#include "EpdRenderer.h"

#include "builtinFonts/babyblue.h"
#include "builtinFonts/bookerly_2b.h"
#include "builtinFonts/bookerly_bold_2b.h"
#include "builtinFonts/bookerly_bold_italic_2b.h"
#include "builtinFonts/bookerly_italic_2b.h"
#include "builtinFonts/ubuntu_10.h"
#include "builtinFonts/ubuntu_bold_10.h"

EpdFont bookerlyFont(&bookerly_2b);
EpdFont bookerlyBoldFont(&bookerly_bold_2b);
EpdFont bookerlyItalicFont(&bookerly_italic_2b);
EpdFont bookerlyBoldItalicFont(&bookerly_bold_italic_2b);
EpdFontFamily bookerlyFontFamily(&bookerlyFont, &bookerlyBoldFont, &bookerlyItalicFont, &bookerlyBoldItalicFont);

EpdFont smallFont(&babyblue);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ubuntu10Font(&ubuntu_10);
EpdFont ununtuBold10Font(&ubuntu_bold_10);
EpdFontFamily ubuntuFontFamily(&ubuntu10Font, &ununtuBold10Font);

EpdRenderer::EpdRenderer(EInkDisplay& einkDisplay)
    : einkDisplay(einkDisplay),
      marginTop(11),
      marginBottom(30),
      marginLeft(10),
      marginRight(10),
      fontRendererMode(BW),
      lineCompression(0.95f) {
  this->regularFontRenderer = new EpdFontRenderer<EInkDisplay>(&bookerlyFontFamily, einkDisplay);
  this->smallFontRenderer = new EpdFontRenderer<EInkDisplay>(&smallFontFamily, einkDisplay);
  this->uiFontRenderer = new EpdFontRenderer<EInkDisplay>(&ubuntuFontFamily, einkDisplay);
}

EpdRenderer::~EpdRenderer() {
  delete regularFontRenderer;
  delete smallFontRenderer;
  delete uiFontRenderer;
}

void EpdRenderer::drawPixel(const int x, const int y, const bool state) const {
  uint8_t* frameBuffer = einkDisplay.getFrameBuffer();

  // Early return if no framebuffer is set
  if (!frameBuffer) {
    Serial.printf("!!No framebuffer\n");
    return;
  }

  const int adjX = x + marginLeft;
  const int adjY = y + marginTop;

  // Bounds checking (portrait: 480x800)
  if (adjX < 0 || adjX >= EInkDisplay::DISPLAY_HEIGHT || adjY < 0 || adjY >= EInkDisplay::DISPLAY_WIDTH) {
    Serial.printf("!!Outside range (%d, %d)\n", adjX, adjY);
    return;
  }

  // Rotate coordinates: portrait (480x800) -> landscape (800x480)
  // Rotation: 90 degrees clockwise
  const int rotatedX = adjY;
  const int rotatedY = EInkDisplay::DISPLAY_HEIGHT - 1 - adjX;

  // Calculate byte position and bit position
  const uint16_t byteIndex = rotatedY * EInkDisplay::DISPLAY_WIDTH_BYTES + (rotatedX / 8);
  const uint8_t bitPosition = 7 - (rotatedX % 8);  // MSB first

  if (state) {
    frameBuffer[byteIndex] &= ~(1 << bitPosition);  // Clear bit
  } else {
    frameBuffer[byteIndex] |= 1 << bitPosition;  // Set bit
  }
}

int EpdRenderer::getTextWidth(const char* text, const EpdFontStyle style) const {
  int w = 0, h = 0;

  regularFontRenderer->fontFamily->getTextDimensions(text, &w, &h, style);

  return w;
}

int EpdRenderer::getUiTextWidth(const char* text, const EpdFontStyle style) const {
  int w = 0, h = 0;

  uiFontRenderer->fontFamily->getTextDimensions(text, &w, &h, style);

  return w;
}

int EpdRenderer::getSmallTextWidth(const char* text, const EpdFontStyle style) const {
  int w = 0, h = 0;

  smallFontRenderer->fontFamily->getTextDimensions(text, &w, &h, style);

  return w;
}

void EpdRenderer::drawText(const int x, const int y, const char* text, const bool state,
                           const EpdFontStyle style) const {
  int ypos = y + getLineHeight() + marginTop;
  int xpos = x + marginLeft;
  regularFontRenderer->renderString(text, &xpos, &ypos, state, style, fontRendererMode);
}

void EpdRenderer::drawUiText(const int x, const int y, const char* text, const bool state,
                             const EpdFontStyle style) const {
  int ypos = y + uiFontRenderer->fontFamily->getData(style)->advanceY + marginTop;
  int xpos = x + marginLeft;
  uiFontRenderer->renderString(text, &xpos, &ypos, state, style, fontRendererMode);
}

void EpdRenderer::drawSmallText(const int x, const int y, const char* text, const bool state,
                                const EpdFontStyle style) const {
  int ypos = y + smallFontRenderer->fontFamily->getData(style)->advanceY + marginTop;
  int xpos = x + marginLeft;
  smallFontRenderer->renderString(text, &xpos, &ypos, state, style, fontRendererMode);
}

void EpdRenderer::drawTextBox(const int x, const int y, const std::string& text, const int width, const int height,
                              const EpdFontStyle style) const {
  const size_t length = text.length();
  // fit the text into the box
  int start = 0;
  int end = 1;
  int ypos = 0;
  while (true) {
    if (end >= length) {
      drawText(x, y + ypos, text.substr(start, length - start).c_str(), true, style);
      break;
    }

    if (ypos + getLineHeight() >= height) {
      break;
    }

    if (text[end - 1] == '\n') {
      drawText(x, y + ypos, text.substr(start, end - start).c_str(), true, style);
      ypos += getLineHeight();
      start = end;
      end = start + 1;
      continue;
    }

    if (getTextWidth(text.substr(start, end - start).c_str(), style) > width) {
      drawText(x, y + ypos, text.substr(start, end - start - 1).c_str(), true, style);
      ypos += getLineHeight();
      start = end - 1;
      continue;
    }

    end++;
  }
}

void EpdRenderer::drawLine(int x1, int y1, int x2, int y2, const bool state) const {
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
    Serial.println("Line drawing not supported");
  }
}

void EpdRenderer::drawRect(const int x, const int y, const int width, const int height, const bool state) const {
  drawLine(x, y, x + width - 1, y, state);
  drawLine(x + width - 1, y, x + width - 1, y + height - 1, state);
  drawLine(x + width - 1, y + height - 1, x, y + height - 1, state);
  drawLine(x, y, x, y + height - 1, state);
}

void EpdRenderer::fillRect(const int x, const int y, const int width, const int height, const bool state) const {
  for (int fillY = y; fillY < y + height; fillY++) {
    drawLine(x, fillY, x + width - 1, fillY, state);
  }
}

void EpdRenderer::drawImage(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  drawImageNoMargin(bitmap, x + marginLeft, y + marginTop, width, height);
}

// TODO: Support y-mirror?
void EpdRenderer::drawImageNoMargin(const uint8_t bitmap[], const int x, const int y, const int width,
                                    const int height) const {
  einkDisplay.drawImage(bitmap, x, y, width, height);
}

void EpdRenderer::clearScreen(const uint8_t color) const {
  Serial.println("Clearing screen");
  einkDisplay.clearScreen(color);
}

void EpdRenderer::invertScreen() const {
  uint8_t *buffer = einkDisplay.getFrameBuffer();
  for (int i = 0; i < EInkDisplay::BUFFER_SIZE; i++) {
    buffer[i] = ~buffer[i];
  }
}

void EpdRenderer::flushDisplay(const EInkDisplay::RefreshMode refreshMode) const {
  einkDisplay.displayBuffer(refreshMode);
}

// TODO: Support partial window update
// void EpdRenderer::flushArea(const int x, const int y, const int width, const int height) const {
//   const int rotatedX = y;
//   const int rotatedY = EInkDisplay::DISPLAY_HEIGHT - 1 - x;
//
//   einkDisplay.displayBuffer(EInkDisplay::FAST_REFRESH, rotatedX, rotatedY, height, width);
// }

int EpdRenderer::getPageWidth() const { return EInkDisplay::DISPLAY_HEIGHT - marginLeft - marginRight; }

int EpdRenderer::getPageHeight() const { return EInkDisplay::DISPLAY_WIDTH - marginTop - marginBottom; }

int EpdRenderer::getSpaceWidth() const { return regularFontRenderer->fontFamily->getGlyph(' ', REGULAR)->advanceX; }

int EpdRenderer::getLineHeight() const {
  return regularFontRenderer->fontFamily->getData(REGULAR)->advanceY * lineCompression;
}

void EpdRenderer::swapBuffers() const { einkDisplay.swapBuffers(); }

void EpdRenderer::copyGrayscaleLsbBuffers() const { einkDisplay.copyGrayscaleLsbBuffers(einkDisplay.getFrameBuffer()); }

void EpdRenderer::copyGrayscaleMsbBuffers() const { einkDisplay.copyGrayscaleMsbBuffers(einkDisplay.getFrameBuffer()); }

void EpdRenderer::displayGrayBuffer() const { einkDisplay.displayGrayBuffer(); }
