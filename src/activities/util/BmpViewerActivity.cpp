#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path,
                                     std::function<void()> onGoBack)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)), onGoBack(std::move(onGoBack)) {}

void BmpViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) return;

  std::string dirPath = "/";
  std::string fileName = filePath;
  size_t lastSlash = filePath.find_last_of('/');
  if (lastSlash != std::string::npos) {
    dirPath = filePath.substr(0, lastSlash);
    if (dirPath.empty()) dirPath = "/";
    fileName = filePath.substr(lastSlash + 1);
  }

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
        if (StringUtils::checkFileExtension(fname, ".bmp")) {
          siblingImages.push_back(fname);
        }
      }
    }
    file.close();
  }
  dir.close();

  // Sort case-insensitively
  std::sort(siblingImages.begin(), siblingImages.end(), [](const std::string& a, const std::string& b) {
    std::string aLow = a, bLow = b;
    std::transform(aLow.begin(), aLow.end(), aLow.begin(), ::tolower);
    std::transform(bLow.begin(), bLow.end(), bLow.begin(), ::tolower);
    return aLow < bLow;
  });

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
      MappedInputManager::Labels labels;
      if (isConfirmingDelete) {
        labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
      } else {
        labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SET_SLEEP_COVER), tr(STR_DELETE), "");
      }
      GUI.fillPopupProgress(renderer, popupRect, 50);

      renderer.clearScreen();
      // Assuming drawBitmap defaults to 0,0 crop if omitted, or pass explicitly: drawBitmap(bitmap, x, y, pageWidth,
      // pageHeight, 0, 0)
      renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);

      if (isConfirmingDelete) {
        GUI.drawPopup(renderer, tr(STR_DELETE_IMAGE_PROMPT));
      }

      // Draw UI hints on the base layer
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      // Single pass for non-grayscale images

      renderer.displayBuffer(HalDisplay::FULL_REFRESH);

    } else {
      // Handle file parsing error
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Invalid BMP File");
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }

    file.close();
  } else {
    // Handle file open error
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Could not open file");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  }
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (isConfirmingDelete) {
      isConfirmingDelete = false;
      onEnter();
    } else {
      if (onGoBack) onGoBack();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (isConfirmingDelete) {
      GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

      if (Storage.remove(filePath.c_str())) {
        GUI.drawPopup(renderer, tr(STR_DONE));
        delay(1000);
        if (onGoBack) onGoBack();
      } else {
        GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
        delay(1000);
        isConfirmingDelete = false;
        onEnter();
      }
      return;
    }

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
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (!isConfirmingDelete) {
      isConfirmingDelete = true;
      onEnter();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (!isConfirmingDelete && siblingImages.size() > 1 && currentImageIndex > 0) {
      currentImageIndex--;
      std::string dirPath = "/";
      size_t lastSlash = filePath.find_last_of('/');
      if (lastSlash != std::string::npos) {
        dirPath = filePath.substr(0, lastSlash);
        if (dirPath.empty()) dirPath = "/";
      }
      if (dirPath.back() != '/') dirPath += "/";
      filePath = dirPath + siblingImages[currentImageIndex];
      onEnter();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (!isConfirmingDelete && siblingImages.size() > 1 && currentImageIndex != -1 &&
        currentImageIndex < static_cast<int>(siblingImages.size()) - 1) {
      currentImageIndex++;
      std::string dirPath = "/";
      size_t lastSlash = filePath.find_last_of('/');
      if (lastSlash != std::string::npos) {
        dirPath = filePath.substr(0, lastSlash);
        if (dirPath.empty()) dirPath = "/";
      }
      if (dirPath.back() != '/') dirPath += "/";
      filePath = dirPath + siblingImages[currentImageIndex];
      onEnter();
    }
    return;
  }
}