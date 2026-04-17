#include "FileBrowserActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "../ActivityManager.h"
#include "../util/ConfirmationActivity.h"
#include "BookInfoActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
}  // namespace

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    // Directories first
    bool isDir1 = str1.back() == '/';
    bool isDir2 = str2.back() == '/';
    if (isDir1 != isDir2) return isDir1;

    // Start naive natural sort
    const char* s1 = str1.c_str();
    const char* s2 = str2.c_str();

    // Iterate while both strings have characters
    while (*s1 && *s2) {
      // Check if both are at the start of a number
      if (isdigit(*s1) && isdigit(*s2)) {
        // Skip leading zeros and track them
        const char* start1 = s1;
        const char* start2 = s2;
        while (*s1 == '0') s1++;
        while (*s2 == '0') s2++;

        // Count digits to compare lengths first
        int len1 = 0, len2 = 0;
        while (isdigit(s1[len1])) len1++;
        while (isdigit(s2[len2])) len2++;

        // Different length so return smaller integer value
        if (len1 != len2) return len1 < len2;

        // Same length so compare digit by digit
        for (int i = 0; i < len1; i++) {
          if (s1[i] != s2[i]) return s1[i] < s2[i];
        }

        // Numbers equal so advance pointers
        s1 += len1;
        s2 += len2;
      } else {
        // Regular case-insensitive character comparison
        char c1 = tolower(*s1);
        char c2 = tolower(*s2);
        if (c1 != c2) return c1 < c2;
        s1++;
        s2++;
      }
    }

    // One string is prefix of other
    return *s1 == '\0' && *s2 != '\0';
  });
}

void FileBrowserActivity::loadFiles() {
  files.clear();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if ((!SETTINGS.showHiddenFiles && name[0] == '.') || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(name) + "/");
    } else {
      std::string_view filename{name};
      if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
          FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
          FsHelpers::hasBmpExtension(filename) || FsHelpers::hasJpgExtension(filename) ||
          FsHelpers::hasPngExtension(filename)) {
        files.emplace_back(filename);
      }
    }
    file.close();
  }
  root.close();
  sortFileList(files);
}

void FileBrowserActivity::onEnter() {
  Activity::onEnter();

  loadFiles();
  selectorIndex = 0;

  if (!focusName.empty()) {
    const size_t idx = findEntry(focusName);
    if (idx < files.size()) {
      selectorIndex = idx;
    }
    focusName.clear();
  }

  requestUpdate();
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
}

void FileBrowserActivity::clearFileMetadata(const std::string& fullPath) {
  // Only clear cache for .epub files
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crosspoint").clearCache();
    LOG_DBG("FileBrowser", "Cleared metadata cache for: %s", fullPath.c_str());
  }
}

void FileBrowserActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

  // Long press BACK always navigates to home
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS) {
    onGoHome();
    return;
  }

  // Short press BACK goes up one directory (if not root) or home (at root)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < GO_HOME_MS) {
    if (basepath != "/") {
      const std::string oldPath = basepath;
      basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
      if (basepath.empty()) basepath = "/";
      loadFiles();
      const auto pos = oldPath.find_last_of('/');
      const std::string dirName = oldPath.substr(pos + 1) + "/";
      const size_t idx = findEntry(dirName);
      selectorIndex = (idx < files.size()) ? idx : 0;
      requestUpdate();
    } else {
      onGoHome();
    }
    return;
  }

  // Confirm short press opens selected entry; long press does nothing
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() < GO_HOME_MS) {
    if (files.empty()) return;

    const std::string& entry = files[selectorIndex];
    const bool isDirectory = (entry.back() == '/');

    if (isDirectory) {
      if (basepath.back() != '/') basepath += "/";
      basepath += entry.substr(0, entry.length() - 1);
      loadFiles();
      selectorIndex = 0;
      requestUpdate();
    } else {
      std::string fullPath = basepath;
      if (fullPath.back() != '/') fullPath += "/";
      fullPath += entry;
      ReturnHint hint;
      hint.target = ReturnTo::FileBrowser;
      hint.path = basepath;
      hint.selectName = entry;
      activityManager.replaceWithReader(std::move(fullPath), std::move(hint));
    }
    return;
  }

  // Left short press does nothing; long press deletes selected file after confirmation
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (mappedInput.getHeldTime() < GO_HOME_MS || files.empty()) {
      return;
    }

    const std::string& entry = files[selectorIndex];
    const bool isDirectory = (entry.back() == '/');
    if (isDirectory) {
      return;
    }

    std::string cleanBase = basepath;
    if (cleanBase.back() != '/') cleanBase += "/";
    const std::string fullPath = cleanBase + entry;

    auto handler = [this, fullPath](const ActivityResult& res) {
      if (!res.isCancelled) {
        LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
        clearFileMetadata(fullPath);
        if (Storage.remove(fullPath.c_str())) {
          LOG_DBG("FileBrowser", "Deleted successfully");
          loadFiles();
          if (files.empty()) {
            selectorIndex = 0;
          } else if (selectorIndex >= files.size()) {
            selectorIndex = files.size() - 1;
          }
          requestUpdate(true);
        } else {
          LOG_ERR("FileBrowser", "Failed to delete file: %s", fullPath.c_str());
        }
      } else {
        LOG_DBG("FileBrowser", "Delete cancelled by user");
      }
    };

    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE) + std::string("? "), entry),
        handler);
    return;
  }

  // Right opens the info page for epub files
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (files.empty()) return;
    const std::string& entry = files[selectorIndex];
    if (entry.back() != '/' && (FsHelpers::hasEpubExtension(entry) || FsHelpers::hasXtcExtension(entry))) {
      std::string cleanBase = basepath;
      if (cleanBase.back() != '/') cleanBase += "/";
      startActivityForResult(std::make_unique<BookInfoActivity>(renderer, mappedInput, cleanBase + entry),
                             [this](const ActivityResult&) { requestUpdate(); });
    }
    return;
  }

  // Up/Down side buttons navigate the list
  const int listSize = static_cast<int>(files.size());
  buttonNavigator.onRelease({MappedInputManager::Button::Down}, [this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onRelease({MappedInputManager::Button::Up}, [this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onContinuous({MappedInputManager::Button::Down}, [this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onContinuous({MappedInputManager::Button::Up}, [this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

std::string getFileName(std::string filename) {
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  if (SETTINGS.showFileExtensions) {
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

void FileBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);

  std::string folderName = (basepath == "/") ? tr(STR_SD_CARD) : basepath.substr(basepath.rfind('/') + 1);
  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 folderName.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;
  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, contentTop + 20,
                      tr(STR_NO_FILES_FOUND));
  } else {
    GUI.drawList(
        renderer, Rect{contentRect.x, contentTop, contentRect.width, contentHeight}, files.size(), selectorIndex,
        [this](int index) { return getFileName(files[index]); }, nullptr,
        [this](int index) { return UITheme::getFileIcon(files[index]); });
  }

  // Side buttons (Up/Down) navigate; show their hints on the side
  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  // Front buttons: Back=Back(subdir)/Home(root), Confirm=Open, Left=hidden long-press delete, Right=Info
  const bool hasInfo =
      !files.empty() && files[selectorIndex].back() != '/' &&
      (FsHelpers::hasEpubExtension(files[selectorIndex]) || FsHelpers::hasXtcExtension(files[selectorIndex]));
  const auto labels = mappedInput.mapLabels(basepath == "/" ? tr(STR_HOME) : tr(STR_BACK),
                                            files.empty() ? "" : tr(STR_OPEN), "", hasInfo ? tr(STR_INFO) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return files.size();
}