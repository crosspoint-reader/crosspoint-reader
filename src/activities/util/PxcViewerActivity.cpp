#include "PxcViewerActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>

#include "Epub/converters/DirectPixelWriter.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
struct PxcCtx {
  FsFile* file;
  uint32_t dataOffset;
  int width, height;
  MappedInputManager::Labels labels;
};

void pxcRenderCallback(const GfxRenderer& r, const void* raw) {
  const auto* c = static_cast<const PxcCtx*>(raw);
  c->file->seek(c->dataOffset);

  const int bytesPerRow = (c->width + 3) / 4;
  uint8_t* rowBuf = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!rowBuf) {
    LOG_ERR("PXC", "malloc failed for rowBuf (%d bytes, %dx%d)", bytesPerRow, c->width, c->height);
    return;
  }

  DirectPixelWriter pw;
  pw.init(r);

  for (int row = 0; row < c->height; row++) {
    if (c->file->read(rowBuf, bytesPerRow) != bytesPerRow) break;
    pw.beginRow(row);
    for (int col = 0; col < c->width; col++) {
      const uint8_t pv = (rowBuf[col >> 2] >> (6 - (col & 3) * 2)) & 0x03;
      pw.writePixel(pv);
    }
  }
  free(rowBuf);

  GUI.drawButtonHints(const_cast<GfxRenderer&>(r), c->labels.btn1, c->labels.btn2, c->labels.btn3, c->labels.btn4);
}

void pxcLoadingOverlay(const GfxRenderer& r, const void*) {
  constexpr int margin = 15;
  const char* msg = tr(STR_LOADING_POPUP);
  const int y = static_cast<int>(r.getScreenHeight() * 0.075f);
  const int textWidth = r.getTextWidth(UI_12_FONT_ID, msg, EpdFontFamily::BOLD);
  const int w = textWidth + margin * 2;
  const int h = r.getLineHeight(UI_12_FONT_ID) + margin * 2;
  const int x = (r.getScreenWidth() - w) / 2;
  r.fillRect(x - 2, y - 2, w + 4, h + 4, true);
  r.fillRect(x, y, w, h, false);
  r.drawText(UI_12_FONT_ID, x + margin, y + margin - 2, msg, true, EpdFontFamily::BOLD);
}
}  // namespace

PxcViewerActivity::PxcViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("PxcViewer", renderer, mappedInput), filePath(std::move(path)) {}

void PxcViewerActivity::renderPxcToFramebuffer(FsFile& file, uint16_t width, uint16_t height, uint32_t dataOffset) {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  PxcCtx ctx{&file, dataOffset, width, height, labels};
  renderer.renderGrayscaleSinglePass(GfxRenderer::GrayscaleMode::FactoryQuality, &pxcRenderCallback, &ctx,
                                     &pxcLoadingOverlay, nullptr);
}

void PxcViewerActivity::onEnter() {
  Activity::onEnter();

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  FsFile file;
  if (!Storage.openFileForRead("PXC", filePath, file)) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2, "Could not open file");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return;
  }

  uint16_t pxcWidth, pxcHeight;
  if (file.read(&pxcWidth, 2) != 2 || file.read(&pxcHeight, 2) != 2) {
    LOG_ERR("PXC", "Header read failed: %s", filePath.c_str());
    file.close();
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2, "Invalid PXC file");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return;
  }

  if (pxcWidth > screenWidth || pxcHeight > screenHeight) {
    LOG_ERR("PXC", "PXC size %dx%d does not match screen %dx%d", pxcWidth, pxcHeight, screenWidth, screenHeight);
    file.close();
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2, "PXC size mismatch");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return;
  }

  const uint32_t dataOffset = file.position();
  renderPxcToFramebuffer(file, pxcWidth, pxcHeight, dataOffset);

  file.close();

  // Sync BW framebuffer state after factory-gray render so onExit's HALF_REFRESH
  // does a correct differential (controller BW state = white, not stale gray planes).
  renderer.clearScreen();
  renderer.cleanupGrayscaleWithFrameBuffer();
}

void PxcViewerActivity::renderGrayscaleImage() {
  FsFile file;
  if (!Storage.openFileForRead("PXC", filePath, file)) return;

  uint16_t pxcWidth, pxcHeight;
  if (file.read(&pxcWidth, 2) != 2 || file.read(&pxcHeight, 2) != 2) {
    file.close();
    return;
  }

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  if (pxcWidth > screenWidth || pxcHeight > screenHeight) {
    file.close();
    return;
  }

  const uint32_t dataOffset = file.position();
  renderPxcToFramebuffer(file, pxcWidth, pxcHeight, dataOffset);

  file.close();
}

void PxcViewerActivity::onScreenshotRequest() {
  renderGrayscaleImage();
  renderer.clearScreen();
  renderer.cleanupGrayscaleWithFrameBuffer();
}

void PxcViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void PxcViewerActivity::loop() {
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return;
  }
}
