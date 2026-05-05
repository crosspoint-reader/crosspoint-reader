#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

void BmpViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) return;

  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  size_t lastSlash = filePath.find_last_of('/');
  std::string fileName = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;

  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.') {
        std::string fname(name);
        if (fname.length() >= 4 && fname.substr(fname.length() - 4) == ".bmp") {
          siblingImages.push_back(fname);
        }
      }
    }
    file.close();
  }
  dir.close();

  FsHelpers::sortFileList(siblingImages);

  for (size_t i = 0; i < siblingImages.size(); ++i) {
    if (siblingImages[i] == fileName) {
      currentImageIndex = static_cast<int>(i);
      break;
    }
  }
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();

  if (siblingImages.empty() && !filePath.empty()) {
    loadSiblingImages();
  }

  FsFile file;

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress
  // 1. Open the file
  if (Storage.openFileForRead("BMP", filePath, file)) {
    Bitmap bitmap(file, true);

    // 2. Parse headers to get dimensions
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      int x, y;

      if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
        float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
        const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

        if (ratio > screenRatio) {
          // Wider than screen
          x = 0;
          y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
        } else {
          // Taller than screen
          x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
          y = 0;
        }
      } else {
        // Center small images
        x = (pageWidth - bitmap.getWidth()) / 2;
        y = (pageHeight - bitmap.getHeight()) / 2;
      }

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SET_SLEEP_COVER), "", "");

      if (bitmap.hasGreyscale()) {
        struct BmpGrayCtx {
          Bitmap* bitmap;
          int x, y, maxWidth, maxHeight;
          MappedInputManager::Labels labels;
        };
        BmpGrayCtx grayCtx{&bitmap, x, y, pageWidth, pageHeight, labels};
        renderer.renderGrayscaleSinglePass(
            GfxRenderer::GrayscaleMode::FactoryQuality,
            [](const GfxRenderer& r, const void* raw) {
              const auto* c = static_cast<const BmpGrayCtx*>(raw);
              r.drawBitmap(*c->bitmap, c->x, c->y, c->maxWidth, c->maxHeight, 0, 0);
              GUI.drawButtonHints(const_cast<GfxRenderer&>(r), c->labels.btn1, c->labels.btn2, c->labels.btn3,
                                  c->labels.btn4);
            },
            &grayCtx,
            [](const GfxRenderer& r, const void*) {
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
            },
            nullptr);
        renderer.clearScreen();
        renderer.cleanupGrayscaleWithFrameBuffer();
      } else {
        GUI.fillPopupProgress(renderer, popupRect, 50);
        renderer.clearScreen();
        renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      }

    } else {
      // Handle file parsing error
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Invalid BMP File");
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }

    file.close();
  } else {
    // Handle file open error
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Could not open file");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}

void BmpViewerActivity::renderGrayscaleImage() {
  FsFile file;
  if (!Storage.openFileForRead("BMP", filePath, file)) return;

  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || !bitmap.hasGreyscale()) {
    file.close();
    return;
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  int x, y;
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
    if (ratio > screenRatio) {
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
    } else {
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
    }
  } else {
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SET_SLEEP_COVER), "", "");
  struct BmpGrayCtx {
    Bitmap* bitmap;
    int x, y, maxWidth, maxHeight;
    MappedInputManager::Labels labels;
  };
  BmpGrayCtx grayCtx{&bitmap, x, y, pageWidth, pageHeight, labels};

  renderer.renderGrayscaleSinglePass(
      GfxRenderer::GrayscaleMode::FactoryQuality,
      [](const GfxRenderer& r, const void* raw) {
        const auto* c = static_cast<const BmpGrayCtx*>(raw);
        r.drawBitmap(*c->bitmap, c->x, c->y, c->maxWidth, c->maxHeight, 0, 0);
        GUI.drawButtonHints(const_cast<GfxRenderer&>(r), c->labels.btn1, c->labels.btn2, c->labels.btn3,
                            c->labels.btn4);
      },
      &grayCtx,
      [](const GfxRenderer& r, const void*) {
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
      },
      nullptr);

  file.close();
}

void BmpViewerActivity::onScreenshotRequest() {
  renderGrayscaleImage();
  renderer.clearScreen();
  renderer.cleanupGrayscaleWithFrameBuffer();
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void BmpViewerActivity::doSetSleepCover() {
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  bool success = false;
  FsFile inFile, outFile;
  if (Storage.openFileForRead("BMP", filePath, inFile)) {
    if (Storage.openFileForWrite("BMP", "/sleep.bmp", outFile)) {
      char buffer[2048];
      int bytesRead;
      success = true;
      while ((bytesRead = inFile.read(buffer, sizeof(buffer))) > 0) {
        if (outFile.write(buffer, bytesRead) != bytesRead) {
          success = false;
          break;
        }
      }
      outFile.close();
    }
    inFile.close();
  }

  if (success) {
    SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    SETTINGS.saveToFile();
    GUI.drawPopup(renderer, tr(STR_DONE));
  } else {
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
  }

  delay(1000);
  onEnter();
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    doSetSleepCover();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (siblingImages.size() > 1 && currentImageIndex > 0) {
      currentImageIndex--;
      std::string dirPath = FsHelpers::extractFolderPath(filePath);
      if (dirPath.back() != '/') dirPath += "/";
      filePath = dirPath + siblingImages[currentImageIndex];
      onEnter();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (siblingImages.size() > 1 && currentImageIndex != -1 &&
        currentImageIndex < static_cast<int>(siblingImages.size()) - 1) {
      currentImageIndex++;
      std::string dirPath = FsHelpers::extractFolderPath(filePath);
      if (dirPath.back() != '/') dirPath += "/";
      filePath = dirPath + siblingImages[currentImageIndex];
      onEnter();
    }
    return;
  }
}
