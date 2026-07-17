#include "FileBrowserActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/BookMover.h"
#include "util/StringUtils.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr size_t NAME_BUFFER_SIZE = 500;
constexpr unsigned long POPUP_MSG_MS = 2000;
}  // namespace

void FileBrowserActivity::loadFiles() {
  files.clear();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    return;
  }

  root.rewindDirectory();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    root.close();
    return;
  }

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
    if ((!SETTINGS.showHiddenFiles && fileNameBuffer[0] == '.') ||
        strcmp(fileNameBuffer.get(), "System Volume Information") == 0) {
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(fileNameBuffer.get()) + "/");
    } else {
      // PickFolder lists the same files as Books mode so the user can tell
      // folders apart; Confirm on one drops the moved book into this folder.
      std::string_view filename{fileNameBuffer.get()};
      if (mode == Mode::PickFirmware) {
        // Firmware picker: only show .bin files.
        if (FsHelpers::checkFileExtension(filename, ".bin")) {
          files.emplace_back(filename);
        }
      } else if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
                 FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
                 FsHelpers::hasBmpExtension(filename)) {
        files.emplace_back(filename);
      }
    }
  }
  root.close();
  FsHelpers::sortFileList(files);
}

void FileBrowserActivity::onEnter() {
  Activity::onEnter();

  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "malloc failed for name buffer");
    return;
  }

  selectorIndex = 0;

  // If Confirm was held while this activity opened (typical when launched from a menu), ignore
  // its release — otherwise we'd immediately auto-open whatever is at index 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  auto root = Storage.open(basepath.c_str());
  if (!root) {
    basepath = "/";
    loadFiles();
  } else if (!root.isDirectory()) {
    lockLongPressBack = mappedInput.isPressed(MappedInputManager::Button::Back);

    const std::string oldPath = basepath;
    basepath = FsHelpers::extractFolderPath(basepath);
    loadFiles();

    const auto pos = oldPath.find_last_of('/');
    const std::string fileName = oldPath.substr(pos + 1);
    selectorIndex = findEntry(fileName);
  } else {
    loadFiles();
  }

  requestUpdate();
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
  fileNameBuffer.reset();
}

// To avoid traversing directories twice (once for cache clearing, once for deletion),
// we do both in one pass here, instead of using Storage.removeDir
bool FileBrowserActivity::removeDirFile(const std::string& fullPath) {
  auto file = Storage.open(fullPath.c_str());
  if (!file) {
    LOG_ERR("FileBrowser", "Failed to open for metadata clearing: %s", fullPath.c_str());
    return false;
  }

  if (!file.isDirectory()) {
    file.close();
    clearBookCache(fullPath);
    return Storage.remove(fullPath.c_str());
  }
  file.close();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    return false;
  }

  // Stack of (dirPath, postOrder): postOrder=true means rmdir this path after children are processed.
  std::vector<std::pair<std::string, bool>> stack;
  stack.reserve(16);
  stack.push_back({fullPath, false});

  while (!stack.empty()) {
    auto [currentPath, postOrder] = std::move(stack.back());
    stack.pop_back();

    if (postOrder) {
      if (!Storage.rmdir(currentPath.c_str())) {
        LOG_ERR("FileBrowser", "Failed to rmdir: %s", currentPath.c_str());
        return false;
      }
      continue;
    }

    auto dir = Storage.open(currentPath.c_str());
    if (!dir) {
      LOG_ERR("FileBrowser", "Failed to open dir: %s", currentPath.c_str());
      return false;
    }
    if (!dir.isDirectory()) {
      LOG_ERR("FileBrowser", "Not a directory: %s", currentPath.c_str());
      return false;
    }

    // Push this dir for post-order rmdir (after all children are processed).
    stack.push_back({currentPath, true});

    dir.rewindDirectory();
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      if (strcmp(fileNameBuffer.get(), ".") == 0 || strcmp(fileNameBuffer.get(), "..") == 0) {
        continue;
      }
      std::string entryPath = currentPath;
      if (entryPath.back() != '/') {
        entryPath += "/";
      }
      entryPath += fileNameBuffer.get();

      const bool isDir = entry.isDirectory();
      entry.close();

      if (isDir) {
        stack.push_back({std::move(entryPath), false});
      } else {
        clearBookCache(entryPath);
        if (!Storage.remove(entryPath.c_str())) {
          LOG_ERR("FileBrowser", "Failed to remove file: %s", entryPath.c_str());
          return false;
        }
      }
    }
  }

  return true;
}

// Defined below render()'s helpers; used here for the menu title.
std::string getFileName(std::string filename);

// Long-press menu on a Books-mode entry: Rename (folders only) / Move / Delete / New folder.
void FileBrowserActivity::showFileMenu(const std::string& entry) {
  const bool isDirectory = entry.back() == '/';
  std::string cleanBasePath = basepath;
  if (cleanBasePath.back() != '/') cleanBasePath += "/";
  const std::string fullPath = cleanBasePath + entry;
  // Move/rename take the path without the trailing slash directory marker.
  std::string cleanFullPath = fullPath;
  if (isDirectory) cleanFullPath.pop_back();

  const char* options[4];
  int count = 0;
  const int renameIdx = isDirectory ? count : -1;
  if (isDirectory) options[count++] = tr(STR_RENAME);
  const int moveIdx = count;
  options[count++] = tr(STR_MOVE);
  const int deleteIdx = count;
  options[count++] = tr(STR_DELETE);
  const int newFolderIdx = count;
  options[count++] = tr(STR_NEW_FOLDER);

  optionPopup.show(
      getFileName(entry).c_str(), options, count, 0,
      [this, entry, fullPath, cleanFullPath, isDirectory, renameIdx, moveIdx, deleteIdx, newFolderIdx](const int sel) {
        if (sel == renameIdx) {
          promptRenameFolder(cleanFullPath);
        } else if (sel == moveIdx) {
          promptMoveDestination(cleanFullPath, isDirectory);
        } else if (sel == deleteIdx) {
          promptDelete(entry, fullPath);
        } else if (sel == newFolderIdx) {
          promptNewFolder();
        }
      });
  requestUpdate();
}

void FileBrowserActivity::promptDelete(const std::string& entry, const std::string& fullPath) {
  auto handler = [this, fullPath](const ActivityResult& res) {
    if (!res.isCancelled) {
      LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
      if (removeDirFile(fullPath)) {
        LOG_DBG("FileBrowser", "Deleted successfully");
        loadFiles();
        if (files.empty()) {
          selectorIndex = 0;
        } else if (selectorIndex >= files.size()) {
          // Move selection to the new "last" item
          selectorIndex = files.size() - 1;
        }

        requestUpdate(true);
      } else {
        LOG_ERR("FileBrowser", "Failed to delete: %s", fullPath.c_str());
      }
    } else {
      LOG_DBG("FileBrowser", "Delete cancelled by user");
    }
  };

  std::string heading = tr(STR_DELETE) + std::string("? ");
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entry), handler);
}

// Opens a destination picker (this same activity in PickFolder mode) and moves
// srcPath there, migrating the book cache(s) so reading progress is kept.
// Folders move with their whole subtree; BookMover::moveFolder rejects a
// destination inside the moved folder itself (surfaces as "Move failed").
void FileBrowserActivity::promptMoveDestination(const std::string& srcPath, const bool isDirectory) {
  startActivityForResult(std::make_unique<FileBrowserActivity>(renderer, mappedInput, basepath, Mode::PickFolder),
                         [this, srcPath, isDirectory](const ActivityResult& res) {
                           if (res.isCancelled) {
                             requestUpdate();
                             return;
                           }
                           std::string dstDir = std::get<FilePathResult>(res.data).path;
                           if (dstDir.size() > 1 && dstDir.back() == '/') dstDir.pop_back();
                           std::string srcDir = srcPath.substr(0, srcPath.find_last_of('/'));
                           if (srcDir.empty()) srcDir = "/";
                           if (dstDir == srcDir) {  // dropped where it already lives: nothing to do
                             requestUpdate();
                             return;
                           }
                           const std::string dstPath = BookMover::buildDestination(srcPath, dstDir);
                           const bool moved = isDirectory ? BookMover::moveFolder(srcPath, dstPath)
                                                          : BookMover::moveFile(srcPath, dstPath);
                           if (moved) {
                             loadFiles();
                             if (files.empty()) {
                               selectorIndex = 0;
                             } else if (selectorIndex >= files.size()) {
                               selectorIndex = files.size() - 1;
                             }
                           } else {
                             showMessage(StrId::STR_MOVE_FAILED);
                           }
                           requestUpdate(true);
                         });
}

// Keyboard prompt prefilled with the folder's current name; renames in place
// via BookMover::moveFolder so books inside keep their reading progress.
void FileBrowserActivity::promptRenameFolder(const std::string& srcPath) {
  const std::string oldName = srcPath.substr(srcPath.find_last_of('/') + 1);
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_FOLDER_NAME), oldName, 64, InputType::Text),
      [this, srcPath, oldName](const ActivityResult& res) {
        if (res.isCancelled) {
          requestUpdate();
          return;
        }
        const std::string name = StringUtils::sanitizeFilename(std::get<KeyboardResult>(res.data).text);
        if (name.empty() || name == oldName) {
          requestUpdate();
          return;
        }
        const std::string dstPath = srcPath.substr(0, srcPath.find_last_of('/') + 1) + name;
        if (Storage.exists(dstPath.c_str()) || !BookMover::moveFolder(srcPath, dstPath)) {
          showMessage(StrId::STR_RENAME_FAILED);
        } else {
          loadFiles();
          selectorIndex = findEntry(name + "/");
        }
        requestUpdate(true);
      });
}

void FileBrowserActivity::promptNewFolder() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_FOLDER_NAME), "", 64, InputType::Text),
      [this](const ActivityResult& res) {
        if (res.isCancelled) {
          requestUpdate();
          return;
        }
        const std::string name = StringUtils::sanitizeFilename(std::get<KeyboardResult>(res.data).text);
        if (name.empty()) {
          requestUpdate();
          return;
        }
        std::string dirPath = basepath;
        if (dirPath.back() != '/') dirPath += '/';
        dirPath += name;
        // An already-existing folder counts as success: the user gets the folder they named.
        auto existing = Storage.open(dirPath.c_str());
        const bool existsAsDir = existing && existing.isDirectory();
        if (existing) existing.close();
        if (!existsAsDir && !Storage.mkdir(dirPath.c_str())) {
          LOG_ERR("FileBrowser", "Failed to create folder: %s", dirPath.c_str());
          showMessage(StrId::STR_FOLDER_CREATE_FAILED);
        } else if (mode == Mode::PickFolder) {
          // Jump straight into the new folder so "Move here" drops the book in it.
          basepath = std::move(dirPath);
          loadFiles();
          selectorIndex = 0;
        } else {
          loadFiles();
          selectorIndex = findEntry(name + "/");
        }
        requestUpdate(true);
      });
}

void FileBrowserActivity::showMessage(const StrId msgId) {
  popupMsgId = msgId;
  popupVisible = true;
  popupTime = millis();
}

void FileBrowserActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (popupVisible && millis() - popupTime >= POPUP_MSG_MS) {
    popupVisible = false;
    requestUpdate(true);
  }

  // Long press BACK (1s+) goes to root folder (Books mode only).
  // In firmware-pick mode we keep navigation simple: short Back = up dir / cancel.
  if (mode == Mode::Books && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= GO_HOME_MS && basepath != "/" && !lockLongPressBack) {
    basepath = "/";
    loadFiles();
    selectorIndex = 0;
    requestUpdate();
    return;
  }

  if (lockLongPressBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockLongPressBack = false;
    return;
  }

  const int pathReserved = renderer.getLineHeight(SMALL_FONT_ID) + UITheme::getInstance().getMetrics().verticalSpacing;
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, pathReserved);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (lockNextConfirmRelease) {
      lockNextConfirmRelease = false;
      return;
    }

    // Folder picker: Confirm enters a directory; on the "Move here" row or on
    // any file it drops the moved book into the folder being viewed (the files
    // are listed so the user can tell folders apart, and landing on one of the
    // books already there is a natural "this is the right shelf" signal).
    if (mode == Mode::PickFolder) {
      if (selectorIndex == 1) {  // "New folder"
        promptNewFolder();
        return;
      }
      const bool onDirectory = selectorIndex >= 2 && files[selectorIndex - 2].back() == '/';
      if (onDirectory) {
        const std::string dirEntry = files[selectorIndex - 2];
        if (basepath.back() != '/') basepath += "/";
        basepath += dirEntry.substr(0, dirEntry.length() - 1);
        loadFiles();
        selectorIndex = 0;
        requestUpdate();
        return;
      }
      // "Move here" row or a file row: return the folder being viewed.
      ActivityResult res{FilePathResult{basepath}};
      res.isCancelled = false;
      setResult(std::move(res));
      finish();
      return;
    }

    if (files.empty()) return;

    const std::string& entry = files[selectorIndex];
    bool isDirectory = (entry.back() == '/');

    // Firmware picker: select file -> return path; navigate into directories normally.
    if (mode == Mode::PickFirmware && !isDirectory) {
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      ActivityResult res{FilePathResult{cleanBasePath + entry}};
      res.isCancelled = false;
      setResult(std::move(res));
      finish();
      return;
    }

    if (mode == Mode::Books && mappedInput.getHeldTime() >= GO_HOME_MS) {
      // --- LONG PRESS ACTION: ENTRY MENU (move / delete / new folder) ---
      showFileMenu(entry);
      return;
    } else {
      // --- SHORT PRESS ACTION: OPEN/NAVIGATE ---
      if (basepath.back() != '/') basepath += "/";

      if (isDirectory) {
        basepath += entry.substr(0, entry.length() - 1);
        loadFiles();
        selectorIndex = 0;
        requestUpdate();
      } else {
        onSelectBook(basepath + entry);
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();

        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = findEntry(dirName) + syntheticCount();
        if (selectorIndex >= files.size() + syntheticCount()) selectorIndex = 0;

        requestUpdate();
      } else if (mode != Mode::Books) {
        // Pickers at root: cancel back to caller instead of going home.
        ActivityResult res;
        res.isCancelled = true;
        setResult(std::move(res));
        finish();
      } else {
        onGoHome();
      }
    }
  }

  int listSize = static_cast<int>(files.size() + syntheticCount());
  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
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
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

std::string getFileExtension(const std::string& filename) {
  if (filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  return filename.substr(pos);
}

void FileBrowserActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName;
  switch (mode) {
    case Mode::PickFirmware:
      folderName = tr(STR_SELECT_FIRMWARE_FILE);
      break;
    case Mode::PickFolder:
      folderName = tr(STR_MOVE_TO);
      break;
    case Mode::Books:
      folderName = (basepath == "/") ? std::string(tr(STR_SD_CARD)) : basepath.substr(basepath.rfind('/') + 1);
      break;
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());

  const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int pathReserved = pathLineHeight + metrics.verticalSpacing;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing - pathReserved;
  const size_t listCount = files.size() + syntheticCount();
  if (listCount == 0) {
    const char* emptyMsg = (mode == Mode::PickFirmware) ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, emptyMsg);
  } else {
    // In PickFolder mode the first two rows are the "Move here" / "New folder"
    // actions; real directory entries follow, shifted by syntheticCount().
    const auto entryAt = [this](const int index) -> const std::string& { return files[index - syntheticCount()]; };
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, listCount, selectorIndex,
        [this, entryAt](int index) {
          if (static_cast<size_t>(index) < syntheticCount()) {
            return std::string(index == 0 ? tr(STR_MOVE_HERE) : tr(STR_NEW_FOLDER));
          }
          return getFileName(entryAt(index));
        },
        nullptr,
        [this, entryAt](int index) {
          // Synthetic action rows borrow the folder icon.
          if (static_cast<size_t>(index) < syntheticCount()) return UITheme::getFileIcon("/");
          return UITheme::getFileIcon(entryAt(index));
        },
        [this, entryAt](int index) {
          if (static_cast<size_t>(index) < syntheticCount()) return std::string();
          return getFileExtension(entryAt(index));
        },
        false);
  }

  // Full path display
  {
    const int pathY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - pathLineHeight;
    const int separatorY = pathY - metrics.verticalSpacing / 2;
    renderer.drawLine(0, separatorY, pageWidth - 1, separatorY, 3, true);
    const int pathMaxWidth = pageWidth - metrics.contentSidePadding * 2;
    // Left-truncate so the deepest directory is always visible
    const char* pathStr = basepath.c_str();
    const char* pathDisplay = pathStr;
    char leftTruncBuf[256];
    if (renderer.getTextWidth(SMALL_FONT_ID, pathStr) > pathMaxWidth) {
      const char ellipsis[] = "\xe2\x80\xa6";  // UTF-8 ellipsis (…)
      const int ellipsisWidth = renderer.getTextWidth(SMALL_FONT_ID, ellipsis);
      const int available = pathMaxWidth - ellipsisWidth;
      // Walk forward from the start until the suffix fits, skipping UTF-8 continuation bytes
      const char* p = pathStr;
      while (*p) {
        if (renderer.getTextWidth(SMALL_FONT_ID, p) <= available) break;
        ++p;
        while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
      }
      snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
      pathDisplay = leftTruncBuf;
    }
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, pathY, pathDisplay);
  }

  // Help text
  const char* backLabel = (basepath == "/") ? (mode == Mode::Books ? tr(STR_HOME) : tr(STR_BACK)) : tr(STR_BACK);
  // In picker modes, Confirm on a selectable row returns to the caller (not "open"); show
  // STR_SELECT instead. Directories in the pickers still descend, so keep STR_OPEN there.
  const bool selectingFirmwareFile = mode == Mode::PickFirmware && !files.empty() && files[selectorIndex].back() != '/';
  const bool syntheticSelected = selectorIndex < syntheticCount();
  // In the folder picker, Confirm over a file drops the moved book here.
  const bool moveHereOnFile = mode == Mode::PickFolder && !syntheticSelected && listCount > 0 &&
                              files[selectorIndex - syntheticCount()].back() != '/';
  const char* confirmLabel = (listCount == 0)                               ? ""
                             : moveHereOnFile                               ? tr(STR_MOVE_HERE)
                             : (selectingFirmwareFile || syntheticSelected) ? tr(STR_SELECT)
                                                                            : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, listCount == 0 ? "" : tr(STR_DIR_UP),
                                            listCount == 0 ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (popupVisible) {
    // drawPopup overlays the framebuffer and refreshes the display itself.
    GUI.drawPopup(renderer, I18N.get(popupMsgId));
  }
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
