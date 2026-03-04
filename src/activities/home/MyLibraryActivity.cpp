#include "MyLibraryActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdlib>

#include "../browser/FileViewerActivity.h"
#include "../util/ConfirmationActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
// Left button held shorter than this is treated as a sort-toggle tap;
// longer holds fall through to ButtonNavigator for navigation-up.
constexpr unsigned long SORT_TAP_MS = 300;
// Virtual path used as basepath when browsing the /Feed/ folder.
// Does not correspond to a real SD card directory.
constexpr const char* VIRTUAL_FEED_PATH = "/Feed";
// Persistent manifest: one full SD path per line, written by RssFeedSync.
constexpr const char* FEED_MANIFEST_FILE = "/.crosspoint/feed_manifest.txt";
// Internal (non-translated) name used in FileEntry for the virtual feed dir.
// findEntry() compares against this so results are language-independent.
constexpr const char* FEED_ENTRY_NAME = "Feed";
}  // namespace

// ---------------------------------------------------------------------------
// Sorting helpers
// ---------------------------------------------------------------------------

static bool naturalLess(const std::string& str1, const std::string& str2) {
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
}

static void sortFileListByName(std::vector<FileEntry>& entries) {
  std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
    if (a.isDirectory != b.isDirectory) return a.isDirectory;
    return naturalLess(a.name, b.name);
  });
}

static void sortFileListByDate(std::vector<FileEntry>& entries) {
  std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
    if (a.isDirectory != b.isDirectory) return a.isDirectory;
    if (a.modTime != b.modTime) return a.modTime > b.modTime;  // newest first
    return naturalLess(a.name, b.name);  // tie-break alphabetically
  });
}

// ---------------------------------------------------------------------------
// File loading helpers
// ---------------------------------------------------------------------------

// Returns packed FAT date+time (date<<16|time) for an open file, or 0 on failure.
static uint32_t getFileModTime(HalFile& file) {
  uint16_t d = 0, t = 0;
  if (file.getModifyDateTime(&d, &t)) {
    return (static_cast<uint32_t>(d) << 16) | t;
  }
  return 0;
}

static bool isSupportedFile(const std::string& filename) {
  return StringUtils::checkFileExtension(filename, ".epub") ||
         StringUtils::checkFileExtension(filename, ".xtch") ||
         StringUtils::checkFileExtension(filename, ".xtc") ||
         StringUtils::checkFileExtension(filename, ".txt") ||
         StringUtils::checkFileExtension(filename, ".md") ||
         StringUtils::checkFileExtension(filename, ".bmp") ||
         StringUtils::checkFileExtension(filename, ".log");
}

// ---------------------------------------------------------------------------
// MyLibraryActivity implementation
// ---------------------------------------------------------------------------

void MyLibraryActivity::loadFiles() {
  files.clear();

  // Virtual /Feed folder: parse the manifest and list those files.
  // Manifest contains one full SD path per line, written by RssFeedSync on each sync.
  if (basepath == VIRTUAL_FEED_PATH) {
    // Use heap-allocated buffer (> 256 bytes per CLAUDE.md malloc rules)
    constexpr size_t MANIFEST_BUF = 4096;
    auto* rawBuf = static_cast<char*>(malloc(MANIFEST_BUF));
    if (!rawBuf) {
      LOG_ERR("MyLibrary", "malloc failed for feed manifest (%u bytes)", static_cast<unsigned>(MANIFEST_BUF));
      return;
    }
    const size_t bytesRead = Storage.readFileToBuffer(FEED_MANIFEST_FILE, rawBuf, MANIFEST_BUF);

    // Parse newline-delimited paths
    char pathBuf[256];
    size_t pathPos = 0;
    for (size_t i = 0; i <= bytesRead; i++) {
      const char c = (i < bytesRead) ? rawBuf[i] : '\n';
      if (c == '\n' || c == '\r') {
        if (pathPos > 0) {
          pathBuf[pathPos] = '\0';
          const std::string fullPath(pathBuf);
          if (isSupportedFile(fullPath) && Storage.exists(fullPath.c_str())) {
            const auto slash = fullPath.rfind('/');
            const std::string fname = (slash != std::string::npos) ? fullPath.substr(slash + 1) : fullPath;
            uint32_t modTime = 0;
            HalFile f = Storage.open(fullPath.c_str());
            if (f) {
              modTime = getFileModTime(f);
              f.close();
            }
            files.push_back({fname, fullPath, modTime, false});
          }
          pathPos = 0;
        }
      } else if (pathPos < sizeof(pathBuf) - 1) {
        pathBuf[pathPos++] = c;
      }
    }
    free(rawBuf);
    rawBuf = nullptr;

    if (sortByDate) {
      sortFileListByDate(files);
    } else {
      sortFileListByName(files);
    }
    return;
  }

  // Regular directory listing
  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }
  root.rewindDirectory();

  // Inject virtual Feed/ entry at root when the manifest file is non-empty.
  // The Feed folder shows files received during the most recent RSS sync.
  if (basepath == "/") {
    HalFile mf = Storage.open(FEED_MANIFEST_FILE);
    if (mf && mf.fileSize() > 0) {
      const uint32_t feedMod = getFileModTime(mf);
      mf.close();
      // Store the internal English name so findEntry() works across languages
      files.push_back({FEED_ENTRY_NAME, VIRTUAL_FEED_PATH, feedMod, true});
    } else {
      if (mf) mf.close();
    }
  }

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }
    if (file.isDirectory()) {
      const uint32_t modTime = getFileModTime(file);
      file.close();
      files.push_back({std::string(name), {}, modTime, true});
    } else {
      const std::string filename(name);
      if (isSupportedFile(filename)) {
        const uint32_t modTime = getFileModTime(file);
        file.close();
        files.push_back({filename, {}, modTime, false});
      } else {
        file.close();
      }
    }
  }
  root.close();

  if (sortByDate) {
    sortFileListByDate(files);
  } else {
    sortFileListByName(files);
  }
}

void MyLibraryActivity::onEnter() {
  Activity::onEnter();
  loadFiles();
  selectorIndex = 0;
  requestUpdate();
}

void MyLibraryActivity::onExit() {
  Activity::onExit();
  files.clear();
}

void MyLibraryActivity::clearFileMetadata(const std::string& fullPath) {
  if (StringUtils::checkFileExtension(fullPath, ".epub")) {
    Epub(fullPath, "/.crosspoint").clearCache();
    LOG_DBG("MyLibrary", "Cleared metadata cache for: %s", fullPath.c_str());
  }
}

// Build the full open path for an entry (uses realPath for virtual /Feed entries).
static std::string entryFullPath(const std::string& basepath, const FileEntry& entry) {
  if (!entry.realPath.empty()) return entry.realPath;
  if (basepath.back() == '/') return basepath + entry.name;
  return basepath + "/" + entry.name;
}

void MyLibraryActivity::loop() {
  // Long press BACK (1s+) goes to root folder
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS &&
      basepath != "/") {
    basepath = "/";
    loadFiles();
    selectorIndex = 0;
    return;
  }

  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (files.empty()) return;
    const FileEntry& entry = files[selectorIndex];

    if (mappedInput.getHeldTime() >= GO_HOME_MS && !entry.isDirectory) {
      // --- LONG PRESS: DELETE FILE ---
      const std::string fullPath = entryFullPath(basepath, entry);
      auto handler = [this, fullPath](const ActivityResult& res) {
        if (!res.isCancelled) {
          LOG_DBG("MyLibrary", "Attempting to delete: %s", fullPath.c_str());
          clearFileMetadata(fullPath);
          if (Storage.remove(fullPath.c_str())) {
            LOG_DBG("MyLibrary", "Deleted successfully");
            loadFiles();
            if (files.empty()) {
              selectorIndex = 0;
            } else if (selectorIndex >= files.size()) {
              selectorIndex = files.size() - 1;
            }
            requestUpdate(true);
          } else {
            LOG_ERR("MyLibrary", "Failed to delete file: %s", fullPath.c_str());
          }
        } else {
          LOG_DBG("MyLibrary", "Delete cancelled by user");
        }
      };
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE) + std::string("? "), entry.name),
          handler);
      return;
    }

    // --- SHORT PRESS: OPEN / NAVIGATE ---
    if (entry.isDirectory) {
      if (entry.realPath.empty()) {
        // Regular directory: append name to basepath
        if (basepath.back() != '/') basepath += "/";
        basepath += entry.name;
      } else {
        // Virtual directory (Feed): use the stored virtual path as the new basepath
        basepath = entry.realPath;
      }
      loadFiles();
      selectorIndex = 0;
      requestUpdate();
    } else {
      const std::string fullPath = entryFullPath(basepath, entry);
      if (StringUtils::isTextViewableFile(entry.name)) {
        startActivityForResult(std::make_unique<FileViewerActivity>(renderer, mappedInput, fullPath), [](auto) {});
      } else {
        onSelectBook(fullPath);
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;
        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();
        const std::string dirName = oldPath.substr(oldPath.find_last_of('/') + 1);
        selectorIndex = findEntry(dirName);
        requestUpdate();
      } else {
        onGoHome();
      }
    }
  }

  // Left short tap (< SORT_TAP_MS) toggles sort order.
  // Longer Left presses fall through to ButtonNavigator for navigation-up.
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && mappedInput.getHeldTime() < SORT_TAP_MS) {
    sortByDate = !sortByDate;
    if (sortByDate) {
      sortFileListByDate(files);
    } else {
      sortFileListByName(files);
    }
    selectorIndex = 0;
    requestUpdate();
    return;
  }

  const int listSize = static_cast<int>(files.size());
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

// Returns display name: translates the virtual feed folder; strips extension from files.
static std::string getDisplayName(const FileEntry& entry) {
  if (entry.isDirectory) {
    if (!entry.realPath.empty() && entry.realPath == VIRTUAL_FEED_PATH) {
      return tr(STR_FEED_FOLDER);
    }
    return entry.name;
  }
  const auto pos = entry.name.rfind('.');
  return (pos != std::string::npos) ? entry.name.substr(0, pos) : entry.name;
}

static UIIcon fileEntryIcon(const FileEntry& entry) {
  if (entry.isDirectory) {
    if (!entry.realPath.empty() && entry.realPath == VIRTUAL_FEED_PATH) return UIIcon::Recent;
    return UIIcon::Folder;
  }
  return UITheme::getFileIcon(entry.name);
}

void MyLibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName;
  if (basepath == "/") {
    folderName = "Browse";
  } else if (basepath == VIRTUAL_FEED_PATH) {
    folderName = tr(STR_FEED_FOLDER);
  } else {
    folderName = basepath.substr(basepath.rfind('/') + 1);
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_FILES_FOUND));
  } else {
    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, files.size(), selectorIndex,
                 [this](int index) { return getDisplayName(files[index]); }, nullptr,
                 [this](int index) { return fileEntryIcon(files[index]); });
  }

  // Btn3 (Left): sort toggle — shows what pressing Left will switch TO
  const char* sortLabel = sortByDate ? tr(STR_SORT_NAME) : tr(STR_SORT_DATE);
  const bool selectedIsText = !files.empty() && selectorIndex < files.size() &&
                              !files[selectorIndex].isDirectory &&
                              StringUtils::isTextViewableFile(files[selectorIndex].name);
  const auto labels =
      mappedInput.mapLabels(basepath == "/" ? tr(STR_HOME) : tr(STR_BACK),
                            files.empty() ? "" : (selectedIsText ? tr(STR_PREVIEW) : tr(STR_OPEN)), sortLabel,
                            files.empty() ? "" : tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

size_t MyLibraryActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++) {
    if (files[i].name == name) return i;
  }
  return 0;
}
