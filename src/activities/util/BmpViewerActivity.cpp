#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
std::string extractFolderPath(const std::string& filePath) {
  const auto lastSlash = filePath.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash == 0) {
    return "/";
  }
  return filePath.substr(0, lastSlash);
}


void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    const char* s1 = str1.c_str();
    const char* s2 = str2.c_str();

    while (*s1 && *s2) {
      if (isdigit(*s1) && isdigit(*s2)) {
        while (*s1 == '0') s1++;
        while (*s2 == '0') s2++;

        int len1 = 0, len2 = 0;
        while (isdigit(s1[len1])) len1++;
        while (isdigit(s2[len2])) len2++;

        if (len1 != len2) return len1 < len2;

        for (int i = 0; i < len1; i++) {
          if (s1[i] != s2[i]) return s1[i] < s2[i];
        }

        s1 += len1;
        s2 += len2;
      } else {
        char c1 = tolower(*s1);
        char c2 = tolower(*s2);
        if (c1 != c2) return c1 < c2;
        s1++;
        s2++;
      }
    }

    return *s1 == '\0' && *s2 != '\0';
  });
}
}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

void BmpViewerActivity::loadSiblingFiles() {
  siblingFiles.clear();
  currentIndex = 0;

  const auto folderPath = extractFolderPath(filePath);
  auto root = Storage.open(folderPath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    siblingFiles.push_back(filePath);
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || file.isDirectory()) {
      file.close();
      continue;
    }

    std::string_view filename{name};
    if (FsHelpers::hasBmpExtension(filename)) {
      if (folderPath == "/") {
        siblingFiles.emplace_back("/" + std::string(filename));
      } else {
        siblingFiles.emplace_back(folderPath + "/" + std::string(filename));
      }
    }
    file.close();
  }
  root.close();

  sortFileList(siblingFiles);

  const auto it = std::find(siblingFiles.begin(), siblingFiles.end(), filePath);
  if (it != siblingFiles.end()) {
    currentIndex = static_cast<size_t>(std::distance(siblingFiles.begin(), it));
  } else if (!siblingFiles.empty()) {
    siblingFiles.push_back(filePath);
    sortFileList(siblingFiles);
    const auto refound = std::find(siblingFiles.begin(), siblingFiles.end(), filePath);
    currentIndex = refound == siblingFiles.end() ? 0 : static_cast<size_t>(std::distance(siblingFiles.begin(), refound));
  } else {
    siblingFiles.push_back(filePath);
  }
}

bool BmpViewerActivity::selectAdjacentFile(int direction) {
  if (siblingFiles.size() <= 1) {
    return false;
  }

  const auto size = static_cast<int>(siblingFiles.size());
  const auto nextIndex = (static_cast<int>(currentIndex) + direction + size) % size;
  if (nextIndex == static_cast<int>(currentIndex)) {
    return false;
  }

  currentIndex = static_cast<size_t>(nextIndex);
  filePath = siblingFiles[currentIndex];
  return true;
}

bool BmpViewerActivity::setCurrentFileAsSleepImage() const {
  const auto pathLen = filePath.size();
  if (pathLen >= sizeof(SETTINGS.sleepScreenImagePath)) {
    LOG_ERR("BMP", "Sleep image path too long: %s", filePath.c_str());
    return false;
  }

  std::strncpy(SETTINGS.sleepScreenImagePath, filePath.c_str(), sizeof(SETTINGS.sleepScreenImagePath) - 1);
  SETTINGS.sleepScreenImagePath[sizeof(SETTINGS.sleepScreenImagePath) - 1] = '\0';
  SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::IMAGE_FROM_FOLDER;
  return SETTINGS.saveToFile();
}

void BmpViewerActivity::renderMessage(const char* message, bool fullRefresh) const {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, message);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(fullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
}

void BmpViewerActivity::renderCurrentImage(bool fullRefresh) const {
  FsFile file;

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);

  if (!Storage.openFileForRead("BMP", filePath, file)) {
    renderMessage("Could not open file", true);
    return;
  }

  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    file.close();
    renderMessage("Invalid BMP File", false);
    return;
  }

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

  const bool hasAdjacentFiles = siblingFiles.size() > 1;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SET_SLEEP), hasAdjacentFiles ? tr(STR_DIR_LEFT) : "",
                                            hasAdjacentFiles ? tr(STR_DIR_RIGHT) : "");
  GUI.fillPopupProgress(renderer, popupRect, 50);

  renderer.clearScreen();
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(fullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);

  file.close();
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();
  loadSiblingFiles();
  renderCurrentImage(true);
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BmpViewerActivity::loop() {
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (setCurrentFileAsSleepImage()) {
      renderMessage("Sleep image set", false);
      delay(500);
      renderCurrentImage(false);
    } else {
      renderMessage("Failed to save", false);
      delay(700);
      renderCurrentImage(false);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (selectAdjacentFile(-1)) {
      renderCurrentImage(false);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (selectAdjacentFile(1)) {
      renderCurrentImage(false);
    }
    return;
  }
}
