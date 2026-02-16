#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include "components/UITheme.h"
#include "fontIds.h"

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path,
                                     std::function<void()> onGoBack)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)), onGoBack(std::move(onGoBack)) {}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();
  renderer.clearScreen();

  FsFile file;
  if (Storage.openFileForRead("BMP", filePath, file)) {
    Bitmap bmp(file, true);

    if (bmp.parseHeaders() == BmpReaderError::Ok) {
      int32_t bmpW = bmp.getWidth();
      int32_t bmpH = bmp.getHeight();

      // Calculate centering
      int32_t xOffset = (renderer.getScreenWidth() - bmpW) / 2;
      int32_t yOffset = (renderer.getScreenHeight() - bmpH) / 2;

      if (xOffset < 0) xOffset = 0;
      if (yOffset < 0) yOffset = 0;

      renderer.drawBitmap(bmp, xOffset, yOffset, bmpW, bmpH);

    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, "Invalid BMP File");
    }
    file.close();
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, "Could not open file");
  }

  const auto labels = mappedInput.mapLabels("« Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
}

void BmpViewerActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (onGoBack) onGoBack();
    return;
  }
}