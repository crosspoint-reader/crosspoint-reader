#include "EtchASketchActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void EtchASketchActivity::onEnter() {
  Activity::onEnter();
  canvasW = renderer.getScreenWidth();
  canvasH = renderer.getScreenHeight();
  cursorX = canvasW / 2;
  cursorY = canvasH / 2;
  penDown = true;

  canvas = new (std::nothrow) uint8_t[CANVAS_BYTES];
  if (canvas) {
    clearCanvas();
  }
  requestUpdate();
}

void EtchASketchActivity::onExit() {
  Activity::onExit();
  delete[] canvas;
  canvas = nullptr;
}

void EtchASketchActivity::setPixel(int x, int y) {
  if (!canvas || x < 0 || x >= canvasW || y < 0 || y >= canvasH) return;
  int idx = y * canvasW + x;
  canvas[idx / 8] |= (1 << (idx % 8));
}

bool EtchASketchActivity::getPixel(int x, int y) const {
  if (!canvas || x < 0 || x >= canvasW || y < 0 || y >= canvasH) return false;
  int idx = y * canvasW + x;
  return (canvas[idx / 8] >> (idx % 8)) & 1;
}

void EtchASketchActivity::clearCanvas() {
  if (canvas) {
    memset(canvas, 0, CANVAS_BYTES);
  }
}

void EtchASketchActivity::saveToBmp() {
  RenderLock lock(*this);

  // Create directory
  Storage.mkdir("/biscuit");
  Storage.mkdir("/biscuit/drawings");

  // Generate filename
  char filename[64];
  snprintf(filename, sizeof(filename), "/biscuit/drawings/sketch_%lu.bmp", millis());

  auto file = Storage.open(filename, O_WRITE | O_CREAT | O_TRUNC);
  if (!file) {
    LOG_ERR("EtchASketch", "Failed to create file: %s", filename);
    return;
  }

  // Write BMP header (1-bit monochrome)
  int rowBytes = ((canvasW + 31) / 32) * 4;  // BMP rows are padded to 4 bytes
  int imageSize = rowBytes * canvasH;
  int fileSize = 62 + imageSize;  // 14 (file header) + 40 (info header) + 8 (palette) + image

  // File header
  uint8_t header[62] = {};
  header[0] = 'B';
  header[1] = 'M';
  header[2] = fileSize & 0xFF;
  header[3] = (fileSize >> 8) & 0xFF;
  header[4] = (fileSize >> 16) & 0xFF;
  header[5] = (fileSize >> 24) & 0xFF;
  header[10] = 62;  // offset to pixel data

  // Info header
  header[14] = 40;  // header size
  header[18] = canvasW & 0xFF;
  header[19] = (canvasW >> 8) & 0xFF;
  header[22] = canvasH & 0xFF;
  header[23] = (canvasH >> 8) & 0xFF;
  header[26] = 1;   // planes
  header[28] = 1;   // bits per pixel
  header[34] = imageSize & 0xFF;
  header[35] = (imageSize >> 8) & 0xFF;
  header[36] = (imageSize >> 16) & 0xFF;
  header[37] = (imageSize >> 24) & 0xFF;

  // Color palette: index 0 = white, index 1 = black
  header[54] = 0xFF;
  header[55] = 0xFF;
  header[56] = 0xFF;
  header[57] = 0x00;
  // index 1 = black (all zeros, already set)

  file.write(header, 62);

  // Write pixel data (BMP is bottom-up)
  uint8_t* row = new (std::nothrow) uint8_t[rowBytes];
  if (row) {
    for (int y = canvasH - 1; y >= 0; y--) {
      memset(row, 0, rowBytes);
      for (int x = 0; x < canvasW; x++) {
        if (getPixel(x, y)) {
          row[x / 8] |= (0x80 >> (x % 8));
        }
      }
      file.write(row, rowBytes);
    }
    delete[] row;
  }

  file.close();
  LOG_DBG("EtchASketch", "Saved to %s", filename);
}

void EtchASketchActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Confirm = save
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    saveToBmp();
    return;
  }

  // PageForward = toggle pen
  if (mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
    penDown = !penDown;
    requestUpdate();
    return;
  }

  bool moved = false;

  if (mappedInput.isPressed(MappedInputManager::Button::Up)) {
    if (cursorY > 0) {
      cursorY--;
      moved = true;
    }
  }
  if (mappedInput.isPressed(MappedInputManager::Button::Down)) {
    if (cursorY < canvasH - 1) {
      cursorY++;
      moved = true;
    }
  }
  if (mappedInput.isPressed(MappedInputManager::Button::Left)) {
    if (cursorX > 0) {
      cursorX--;
      moved = true;
    }
  }
  if (mappedInput.isPressed(MappedInputManager::Button::Right)) {
    if (cursorX < canvasW - 1) {
      cursorX++;
      moved = true;
    }
  }

  if (moved) {
    if (penDown) {
      setPixel(cursorX, cursorY);
    }
    requestUpdate();
  }
}

void EtchASketchActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (!canvas) {
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_OUT_OF_MEMORY));
    renderer.displayBuffer();
    return;
  }

  // Draw canvas
  for (int y = 0; y < canvasH; y++) {
    for (int x = 0; x < canvasW; x++) {
      if (getPixel(x, y)) {
        renderer.drawPixel(x, y, true);
      }
    }
  }

  // Draw cursor crosshair (XOR-like: draw inverted)
  const int crossSize = 5;
  for (int i = -crossSize; i <= crossSize; i++) {
    int px = cursorX + i;
    int py = cursorY;
    if (px >= 0 && px < canvasW) {
      renderer.drawPixel(px, py, !getPixel(px, py));
    }
    px = cursorX;
    py = cursorY + i;
    if (py >= 0 && py < canvasH) {
      renderer.drawPixel(px, py, !getPixel(px, py));
    }
  }

  // Pen state indicator at top-left
  const char* penLabel = penDown ? tr(STR_PEN_DOWN) : tr(STR_PEN_UP);
  renderer.fillRect(0, 0, renderer.getTextWidth(SMALL_FONT_ID, penLabel) + 8, 15, false);
  renderer.drawText(SMALL_FONT_ID, 4, 2, penLabel);

  renderer.displayBuffer();
}
