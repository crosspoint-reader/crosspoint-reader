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
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

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
      if (mode == Mode::PickFirmware) {
        if (FsHelpers::checkFileExtension(filename, ".bin")) {
          files.emplace_back(filename);
        }
      } else if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
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
      selectorIndex = static_cast<int>(idx);
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
  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Back) {
      if (ev.type == ButtonEventManager::PressType::Long) {
        if (mode == Mode::Books) {
          onGoHome();
          return;
        }
        // PickFirmware: long Back = same as short Back (cancel / up dir)
      }
      if (ev.type == ButtonEventManager::PressType::Short || ev.type == ButtonEventManager::PressType::Long) {
        if (basepath != "/") {
          const std::string oldPath = basepath;
          basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
          if (basepath.empty()) basepath = "/";
          loadFiles();
          const auto pos = oldPath.find_last_of('/');
          const std::string dirName = oldPath.substr(pos + 1) + "/";
          const size_t idx = findEntry(dirName);
          selectorIndex = (idx < files.size()) ? static_cast<int>(idx) : 0;
          requestUpdate();
        } else if (mode == Mode::PickFirmware) {
          // At root in PickFirmware: cancel back to caller.
          ActivityResult res;
          res.isCancelled = true;
          setResult(std::move(res));
          finish();
        } else {
          onGoHome();
        }
        return;
      }
    }

    if (ev.button == MappedInputManager::Button::Confirm &&
        (ev.type == ButtonEventManager::PressType::Short || ev.type == ButtonEventManager::PressType::Long)) {
      if (files.empty()) return;

      const std::string& entry = files[selectorIndex];
      const bool isDirectory = (entry.back() == '/');
      const bool longPress = (ev.type == ButtonEventManager::PressType::Long);

      if (isDirectory) {
        // Long press on a directory has no useful sync action; ignore.
        if (longPress) return;
        if (basepath.back() != '/') basepath += "/";
        basepath += entry.substr(0, entry.length() - 1);
        loadFiles();
        selectorIndex = 0;
        requestUpdate();
      } else if (mode == Mode::PickFirmware) {
        // Firmware picker: return the selected path to the caller.
        std::string cleanBasePath = basepath;
        if (cleanBasePath.back() != '/') cleanBasePath += "/";
        ActivityResult res{FilePathResult{cleanBasePath + entry}};
        res.isCancelled = false;
        setResult(std::move(res));
        finish();
        return;
      } else {
        std::string fullPath = basepath;
        if (fullPath.back() != '/') fullPath += "/";
        fullPath += entry;
        // Long-press on an EPUB arms an AUTO_PULL before the reader renders its first page.
        // Restricted to EPUB so long-pressing a non-EPUB cannot leak the flag to the reader.
        if (longPress && KOREADER_STORE.hasCredentials() && FsHelpers::hasEpubExtension(fullPath)) {
          auto& sync = APP_STATE.koReaderSyncSession;
          sync.autoPullEpubPath = fullPath;
          sync.exitToHomeAfterSync = false;
          APP_STATE.saveToFile();
        }
        ReturnHint hint;
        hint.target = ReturnTo::FileBrowser;
        hint.path = basepath;
        hint.selectName = entry;
        activityManager.replaceWithReader(std::move(fullPath), std::move(hint));
      }
      return;
    }

    if (ev.button == MappedInputManager::Button::Left && ev.type == ButtonEventManager::PressType::Long) {
      if (files.empty()) {
        return;
      }

      const std::string& entry = files[selectorIndex];
      const bool isDirectory = (entry.back() == '/');
      std::string entryName = entry;
      if (isDirectory) {
        entryName.pop_back();
      }

      std::string cleanBase = basepath;
      if (cleanBase.back() != '/') cleanBase += "/";
      const std::string fullPath = cleanBase + entryName;

      auto handler = [this, fullPath, isDirectory](const ActivityResult& res) {
        if (!res.isCancelled) {
          LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
          clearFileMetadata(fullPath);
          const bool deleted = isDirectory ? Storage.removeDir(fullPath.c_str()) : Storage.remove(fullPath.c_str());
          if (deleted) {
            LOG_DBG("FileBrowser", "Deleted successfully");
            loadFiles();
            if (files.empty()) {
              selectorIndex = 0;
            } else if (selectorIndex >= static_cast<int>(files.size())) {
              selectorIndex = static_cast<int>(files.size()) - 1;
            }
            requestUpdate(true);
          } else {
            LOG_ERR("FileBrowser", "Failed to delete: %s", fullPath.c_str());
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

    if (ev.button == MappedInputManager::Button::Right && ev.type == ButtonEventManager::PressType::Short) {
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
  }

  // Up/Down side buttons navigate the list (Left/Right are reserved for Back/Info actions)
  const int listSize = static_cast<int>(files.size());
  buttonNavigator.onNextList({MappedInputManager::Button::Down}, selectorIndex, listSize, [this] { requestUpdate(); });
  buttonNavigator.onPreviousList({MappedInputManager::Button::Up}, selectorIndex, listSize,
                                 [this] { requestUpdate(); });
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

  std::string folderName =
      (mode == Mode::PickFirmware)
          ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
          : ((basepath == "/") ? std::string(tr(STR_SD_CARD)) : basepath.substr(basepath.rfind('/') + 1));
  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 folderName.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;
  if (files.empty()) {
    const char* emptyMsg = (mode == Mode::PickFirmware) ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND);
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, contentTop + 20, emptyMsg);
  } else {
    GUI.drawList(
        renderer, Rect{contentRect.x, contentTop, contentRect.width, contentHeight}, files.size(), selectorIndex,
        [this](int index) { return getFileName(files[index]); }, nullptr,
        [this](int index) { return UITheme::getFileIcon(files[index]); });
  }

  // Side buttons (Up/Down) navigate; show their hints on the side
  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  // Front buttons
  const char* backLabel = (basepath == "/") ? (mode == Mode::PickFirmware ? tr(STR_BACK) : tr(STR_HOME)) : tr(STR_BACK);
  const bool selectingFirmwareFile = mode == Mode::PickFirmware && !files.empty() && files[selectorIndex].back() != '/';
  const char* confirmLabel = files.empty() ? "" : (selectingFirmwareFile ? tr(STR_SELECT) : tr(STR_OPEN));
  const bool hasInfo =
      mode == Mode::Books && !files.empty() && files[selectorIndex].back() != '/' &&
      (FsHelpers::hasEpubExtension(files[selectorIndex]) || FsHelpers::hasXtcExtension(files[selectorIndex]));
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, "", hasInfo ? tr(STR_INFO) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return files.size();
}