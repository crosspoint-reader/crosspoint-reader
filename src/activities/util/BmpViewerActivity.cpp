#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
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

      // 4. Prepare Rendering
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SET_SLEEP_COVER), "", "");

      GUI.fillPopupProgress(renderer, popupRect, 50);

      renderer.clearScreen();
      // Assuming drawBitmap defaults to 0,0 crop if omitted, or pass explicitly: drawBitmap(bitmap, x, y, pageWidth,
      // pageHeight, 0, 0)
      renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);

      // Draw UI hints on the base layer
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      // Single pass for non-grayscale images

      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

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