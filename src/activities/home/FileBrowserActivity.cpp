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
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr size_t NAME_BUFFER_SIZE = 500;
// Folders with more entries than this are browsed through the on-SD FileIndex
// instead of an in-RAM vector, keeping heap use bounded (~25 KB worst case
// here) no matter how many files a directory holds.
constexpr size_t INDEX_THRESHOLD = 256;

bool isSupportedFile(std::string_view name) {
  return FsHelpers::hasEpubExtension(name) || FsHelpers::hasXtcExtension(name) || FsHelpers::hasTxtExtension(name) ||
         FsHelpers::hasMarkdownExtension(name) || FsHelpers::hasBmpExtension(name);
}

bool isDirName(const char* name) {
  const size_t len = strlen(name);
  return len > 0 && name[len - 1] == '/';
}

// Extension for type sort: suffix after the last dot, empty for directories
// and dot-less or dot-leading names.
const char* extensionOf(const char* name) {
  if (isDirName(name)) return "";
  const char* dot = strrchr(name, '.');
  if (dot == nullptr || dot == name) return "";
  return dot + 1;
}

// Entry filters shared between the in-RAM listing and the FileIndex backend
// (the index hashes accepted entries into its staleness signature, so a filter
// change — e.g. toggling hidden files — naturally triggers a rebuild).
bool acceptCommon(const char* name, bool isDir) {
  if (!SETTINGS.showHiddenFiles && name[0] == '.') return false;
  if (strcmp(name, "System Volume Information") == 0) return false;
  if (isDir) return true;
  return isSupportedFile(name);
}

bool acceptFirmware(const char* name, bool isDir) {
  if (!SETTINGS.showHiddenFiles && name[0] == '.') return false;
  if (strcmp(name, "System Volume Information") == 0) return false;
  if (isDir) return true;
  return FsHelpers::checkFileExtension(std::string_view{name}, ".bin");
}
}  // namespace

void FileBrowserActivity::sortFileList() {
  const auto sortMode = SETTINGS.fileSortMode;
  const bool descending = SETTINGS.fileSortDirection == CrossPointSettings::SORT_DESC;
  // Safe to capture: the arena does not grow while sorting
  const char* arena = nameArena.data();

  std::sort(begin(files), end(files), [arena, sortMode, descending](const FileEntry& a, const FileEntry& b) {
    const char* an = arena + a.nameOffset;
    const char* bn = arena + b.nameOffset;

    // Directories first, regardless of sort mode or direction
    const bool aDir = isDirName(an);
    const bool bDir = isDirName(bn);
    if (aDir != bDir) return aDir;

    int cmp;
    switch (sortMode) {
      case CrossPointSettings::SORT_DATE:
        cmp = (a.dateTime != b.dateTime) ? (a.dateTime < b.dateTime ? -1 : 1) : FsHelpers::naturalCompare(an, bn);
        break;
      case CrossPointSettings::SORT_SIZE:
        cmp = (a.size != b.size) ? (a.size < b.size ? -1 : 1) : FsHelpers::naturalCompare(an, bn);
        break;
      case CrossPointSettings::SORT_TYPE:
        cmp = FsHelpers::naturalCompare(extensionOf(an), extensionOf(bn));
        if (cmp == 0) cmp = FsHelpers::naturalCompare(an, bn);
        break;
      default:  // SORT_NAME
        cmp = FsHelpers::naturalCompare(an, bn);
        break;
    }
    return descending ? (cmp > 0) : (cmp < 0);
  });
}

// Stream directory entries into `files`, stopping once `cap` entries are
// collected (overflow=true). Returns false if the directory cannot be opened.
bool FileBrowserActivity::loadFilesIntoVector(size_t cap, bool& overflow) {
  files.clear();
  nameArena.clear();
  overflow = false;

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    return false;
  }

  root.rewindDirectory();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    root.close();
    return false;
  }

  const auto accept = (mode == Mode::PickFirmware) ? acceptFirmware : acceptCommon;

  files.reserve(64);
  nameArena.reserve(2048);
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
    const bool isDir = file.isDirectory();
    if (!accept(fileNameBuffer.get(), isDir)) {
      continue;
    }

    if (files.size() >= cap) {
      overflow = true;
      break;
    }

    // FAT timestamp for date sort: take max(mtime, ctime) so files copied onto the
    // card (which keep their original mtime but get a fresh ctime) sort by when
    // they were added to the device.
    uint16_t mdate = 0, mtime = 0, cdate = 0, ctime = 0;
    file.getModifyDateTime(&mdate, &mtime);
    file.getCreateDateTime(&cdate, &ctime);
    const uint32_t modTs = (static_cast<uint32_t>(mdate) << 16) | mtime;
    const uint32_t crtTs = (static_cast<uint32_t>(cdate) << 16) | ctime;
    const uint32_t dateTime = modTs > crtTs ? modTs : crtTs;

    const char* namePtr = fileNameBuffer.get();
    const uint32_t nameOffset = static_cast<uint32_t>(nameArena.size());
    nameArena.insert(nameArena.end(), namePtr, namePtr + strlen(namePtr));
    if (isDir) nameArena.push_back('/');
    nameArena.push_back('\0');

    files.push_back({nameOffset, isDir ? 0 : static_cast<uint32_t>(file.fileSize()), dateTime});
  }
  root.close();
  return true;
}

void FileBrowserActivity::loadFiles() {
  usingIndex = false;
  indexCachedRow = SIZE_MAX;
  if (fileIndex) fileIndex->close();
  sortDescending = SETTINGS.fileSortDirection == CrossPointSettings::SORT_DESC;

  bool overflow = false;
  if (!loadFilesIntoVector(INDEX_THRESHOLD, overflow)) {
    return;
  }

  if (!overflow) {
    sortFileList();
    return;
  }

  // Folder is larger than the in-RAM budget: switch to the on-SD index.
  // Free the partial listing first — the index build needs only a few KB.
  files.clear();
  files.shrink_to_fit();
  nameArena.clear();
  nameArena.shrink_to_fit();

  if (!fileIndex) fileIndex = makeUniqueNoThrow<FileIndex>();
  if (!indexEntry) indexEntry = makeUniqueNoThrow<FileIndex::Entry>();
  if (fileIndex && indexEntry) {
    // Validating (and possibly rebuilding) the index re-scans the directory,
    // which can take seconds on huge folders — show the busy popup. Scoped
    // lock: drawPopup pushes to the display, which the render task also owns.
    {
      RenderLock lock(*this);
      GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    }
    const auto sortMode = static_cast<FileIndex::SortMode>(SETTINGS.fileSortMode);
    const auto accept = (mode == Mode::PickFirmware) ? acceptFirmware : acceptCommon;
    if (fileIndex->open(basepath.c_str(), sortMode, accept)) {
      usingIndex = true;
      requestUpdate(true);
      return;
    }
  }

  // Index unavailable (allocation or IO failure): degrade to a capped in-RAM
  // listing instead of crashing on a huge folder.
  LOG_ERR("FileBrowser", "index unavailable for %s; showing first %u entries", basepath.c_str(),
          static_cast<unsigned>(INDEX_THRESHOLD));
  loadFilesIntoVector(INDEX_THRESHOLD, overflow);
  sortFileList();
  requestUpdate(true);  // full refresh clears the busy popup
}

size_t FileBrowserActivity::entryCount() const { return usingIndex ? fileIndex->totalCount() : files.size(); }

const char* FileBrowserActivity::entryNameAt(size_t row) {
  if (!usingIndex) {
    return nameArena.data() + files[row].nameOffset;
  }
  if (row != indexCachedRow) {
    if (!fileIndex->entryAt(row, sortDescending, *indexEntry)) {
      LOG_ERR("FileBrowser", "index read failed at row %u", static_cast<unsigned>(row));
      indexCachedRow = SIZE_MAX;
      return "?";  // never empty: callers inspect the last character
    }
    indexCachedName.assign(indexEntry->name);
    if (indexEntry->isDir) indexCachedName += '/';
    indexCachedRow = row;
  }
  return indexCachedName.c_str();
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
  nameArena.clear();
  nameArena.shrink_to_fit();
  fileIndex.reset();
  indexEntry.reset();
  indexCachedName.clear();
  indexCachedName.shrink_to_fit();
  indexCachedRow = SIZE_MAX;
  usingIndex = false;
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

void FileBrowserActivity::loop() {
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
    if (entryCount() == 0) return;

    const std::string entry = entryNameAt(selectorIndex);
    bool isDirectory = !entry.empty() && entry.back() == '/';

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
      // --- LONG PRESS ACTION: DELETE FILE OR DIRECTORY ---
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      const std::string fullPath = cleanBasePath + entry;

      auto handler = [this, fullPath](const ActivityResult& res) {
        if (!res.isCancelled) {
          LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
          if (removeDirFile(fullPath)) {
            LOG_DBG("FileBrowser", "Deleted successfully");
            loadFiles();
            if (entryCount() == 0) {
              selectorIndex = 0;
            } else if (selectorIndex >= entryCount()) {
              // Move selection to the new "last" item
              selectorIndex = entryCount() - 1;
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
        selectorIndex = findEntry(dirName);

        requestUpdate();
      } else if (mode == Mode::PickFirmware) {
        // Firmware picker at root: cancel back to caller instead of going home.
        ActivityResult res;
        res.isCancelled = true;
        setResult(std::move(res));
        finish();
      } else {
        onGoHome();
      }
    }
  }

  int listSize = static_cast<int>(entryCount());
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

std::string getFileExtension(std::string filename) {
  if (filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  if (pos == std::string::npos) {
    return "";
  }
  return filename.substr(pos);
}

void FileBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName =
      (mode == Mode::PickFirmware)
          ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
          : ((basepath == "/") ? std::string(tr(STR_SD_CARD)) : basepath.substr(basepath.rfind('/') + 1));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());

  const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int pathReserved = pathLineHeight + metrics.verticalSpacing;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing - pathReserved;
  if (entryCount() == 0) {
    const char* emptyMsg = (mode == Mode::PickFirmware) ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, emptyMsg);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, entryCount(), selectorIndex,
        [this](int index) { return getFileName(entryNameAt(index)); }, nullptr,
        [this](int index) { return UITheme::getFileIcon(entryNameAt(index)); },
        [this](int index) { return getFileExtension(entryNameAt(index)); }, false);
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
  const char* backLabel = (basepath == "/") ? (mode == Mode::PickFirmware ? tr(STR_BACK) : tr(STR_HOME)) : tr(STR_BACK);
  // In PickFirmware mode, Confirm on a .bin returns the path to the caller (not "open"); show
  // STR_SELECT instead. Directories in the same picker still descend, so keep STR_OPEN there.
  const bool empty = entryCount() == 0;
  const bool selectingFirmwareFile = mode == Mode::PickFirmware && !empty && !isDirName(entryNameAt(selectorIndex));
  const char* confirmLabel = empty ? "" : (selectingFirmwareFile ? tr(STR_SELECT) : tr(STR_OPEN));
  const auto labels =
      mappedInput.mapLabels(backLabel, confirmLabel, empty ? "" : tr(STR_DIR_UP), empty ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

size_t FileBrowserActivity::findEntry(const std::string& name) {
  if (usingIndex) {
    // Index entries store raw names; strip the directory marker for lookup
    std::string raw = name;
    if (!raw.empty() && raw.back() == '/') raw.pop_back();
    const size_t row = fileIndex->findRowByName(raw.c_str(), sortDescending);
    return row == SIZE_MAX ? 0 : row;
  }
  for (size_t i = 0; i < files.size(); i++)
    if (name == nameArena.data() + files[i].nameOffset) return i;
  return 0;
}
