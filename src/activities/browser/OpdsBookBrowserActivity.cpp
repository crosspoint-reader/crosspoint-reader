#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "../util/ConfirmationActivity.h"
#include "../util/KeyboardEntryActivity.h"
#include "CrossPointSettings.h"
#include "FileBrowserActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
}

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    bool isDir1 = str1.back() == '/';
    bool isDir2 = str2.back() == '/';
    if (isDir1 != isDir2) return isDir1;

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
    files.emplace_back(file.isDirectory() ? std::string(name) + "/" : std::string(name));
    file.close();
  }
  root.close();
  sortFileList(files);
}

void FileBrowserActivity::loadPickerFolders() {
  pickerFolders.clear();
  pickerFiles.clear();
  auto root = Storage.open(pickerPath.c_str());
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
    file.isDirectory() ? pickerFolders.emplace_back(std::string(name) + "/")
                       : pickerFiles.emplace_back(std::string(name));
    file.close();
  }
  root.close();
  sortFileList(pickerFolders);
  sortFileList(pickerFiles);
}

void FileBrowserActivity::performMove(const std::string& destPath) {
  const auto pos = fileToMove.rfind('/');
  const std::string filename = (pos != std::string::npos) ? fileToMove.substr(pos + 1) : fileToMove;
  std::string dest = destPath;
  if (dest.back() != '/') dest += "/";
  dest += filename;

  if (dest == fileToMove) {
    fileToMove.clear();
    mode = BrowserMode::NORMAL;
    requestUpdate(true);
    return;
  }

  bool ok = Storage.rename(fileToMove.c_str(), dest.c_str());
  if (ok)
    clearFileMetadata(fileToMove);
  else {
    // Fallback logic for cross-directory FAT32 moves (Copy + Delete) omitted for brevity as per original logic
  }
  fileToMove.clear();
  mode = BrowserMode::NORMAL;
  loadFiles();
  if (selectorIndex >= files.size() && !files.empty()) selectorIndex = files.size() - 1;
  requestUpdate(true);
}

void FileBrowserActivity::createFolder(const std::string& name) {
  if (name.empty()) return;
  const std::string sanitized = StringUtils::sanitizeFilename(name);
  if (sanitized.empty()) return;
  std::string path = basepath;
  if (path.back() != '/') path += "/";
  path += sanitized;
  Storage.mkdir(path.c_str());
  loadFiles();
  requestUpdate(true);
}

void FileBrowserActivity::onEnter() {
  Activity::onEnter();
  loadFiles();
  selectorIndex = 0;
  requestUpdate();
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
}

void FileBrowserActivity::clearFileMetadata(const std::string& fullPath) {
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crosspoint").clearCache();
  }
}

void FileBrowserActivity::loop() {
  if (mode == BrowserMode::PICKING_FOLDER) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      performMove(pickerPath);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (!pickerFolders.empty()) {
        if (pickerPath.back() != '/') pickerPath += "/";
        const std::string& sel = pickerFolders[pickerIndex];
        pickerPath += sel.substr(0, sel.length() - 1);
        pickerIndex = 0;
        loadPickerFolders();
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (pickerPath != "/") {
        const auto slash = pickerPath.find_last_of('/');
        pickerPath = (slash == 0) ? "/" : pickerPath.substr(0, slash);
        pickerIndex = 0;
        loadPickerFolders();
        requestUpdate();
      } else {
        fileToMove.clear();
        mode = BrowserMode::NORMAL;
        requestUpdate();
      }
      return;
    }
    // Up/Down navigation omitted for brevity
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS &&
      basepath != "/") {
    basepath = "/";
    loadFiles();
    selectorIndex = 0;
    return;
  }

  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (consumeConfirm) {
      consumeConfirm = false;
      return;
    }
    if (files.empty()) return;

    const std::string& entry = files[selectorIndex];
    bool isDirectory = (entry.back() == '/');

    if (mappedInput.getHeldTime() >= GO_HOME_MS) {
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      const std::string entryName = isDirectory ? entry.substr(0, entry.length() - 1) : entry;
      const std::string fullPath = cleanBasePath + entryName;

      auto handler = [this, fullPath, isDirectory](const ActivityResult& res) {
        if (!res.isCancelled) {
          bool ok = isDirectory ? Storage.removeDir(fullPath.c_str())
                                : (clearFileMetadata(fullPath), Storage.remove(fullPath.c_str()));
          if (ok) {
            loadFiles();
            selectorIndex = files.empty() ? 0 : std::min(selectorIndex, files.size() - 1);
            requestUpdate(true);
          }
        }
      };

      consumeBack = true;
      consumeConfirm = true;  // Prevents UI bleed when returning from dialog
      std::string heading = tr(STR_DELETE) + std::string("? ");
      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entryName),
                             handler);
      return;
    } else {
      if (basepath.back() != '/') basepath += "/";
      if (isDirectory) {
        basepath += entry.substr(0, entry.length() - 1);
        loadFiles();
        selectorIndex = 0;
        requestUpdate();
      } else
        onSelectBook(basepath + entry);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (consumeBack) {
      consumeBack = false;
      return;
    }
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;
        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();
        const auto pos = oldPath.find_last_of('/');
        selectorIndex = findEntry(oldPath.substr(pos + 1) + "/");
        requestUpdate();
      } else
        onGoHome();
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (!files.empty() && files[selectorIndex].back() != '/') {
      std::string cleanBase = basepath;
      if (cleanBase.back() != '/') cleanBase += "/";
      fileToMove = cleanBase + files[selectorIndex];
      pickerPath = "/";
      pickerIndex = 0;
      loadPickerFolders();
      mode = BrowserMode::PICKING_FOLDER;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    consumeBack = consumeConfirm = true;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "New Folder"),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) createFolder(std::get<KeyboardResult>(result.data).text);
                           });
    return;
  }

  // List navigation logic omitted for brevity
}

std::string getFileName(std::string filename) {
  if (filename.back() == '/') {
    filename.pop_back();
    return UITheme::getInstance().getTheme().showsFileIcons() ? filename : "[" + filename + "]";
  }
  return filename.substr(0, filename.rfind('.'));
}

void FileBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (mode == BrowserMode::PICKING_FOLDER) {
    const auto pos = fileToMove.rfind('/');
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   (std::string("Move: ") + fileToMove.substr(pos + 1)).c_str());
    // Drawing logic for Picker Mode...
    renderer.displayBuffer();
    return;
  }

  std::string folderName = (basepath == "/") ? tr(STR_SD_CARD) : basepath.substr(basepath.rfind('/') + 1);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_FILES_FOUND));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, files.size(), selectorIndex,
        [this](int index) { return getFileName(files[index]); }, nullptr,
        [this](int index) { return UITheme::getFileIcon(files[index]); });
  }

  const bool selectedIsFile = !files.empty() && files[selectorIndex].back() != '/';
  const auto labels =
      mappedInput.mapLabels(basepath == "/" ? tr(STR_HOME) : tr(STR_BACK), files.empty() ? "" : tr(STR_OPEN),
                            selectedIsFile ? "Move" : "", "New Folder");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
