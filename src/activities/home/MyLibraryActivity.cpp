#include "MyLibraryActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <utility>

#include "../util/ConfirmationActivity.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "ScreenComponents.h"
#include "SpiBusMutex.h"
#include "activities/TaskShutdown.h"
#include "components/UITheme.h"
#include "core/features/FeatureModules.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
// Timing thresholds
constexpr unsigned long GO_HOME_MS = 1000;

std::string fallbackTitleFromPath(const std::string& path) {
  std::string title = path;
  const size_t lastSlash = title.find_last_of('/');
  if (lastSlash != std::string::npos) {
    title = title.substr(lastSlash + 1);
  }

  if (StringUtils::checkFileExtension(title, ".xtch")) {
    title.resize(title.length() - 5);
  } else if (StringUtils::checkFileExtension(title, ".epub") || StringUtils::checkFileExtension(title, ".xtc") ||
             StringUtils::checkFileExtension(title, ".txt") || StringUtils::checkFileExtension(title, ".md")) {
    title.resize(title.length() - 4);
  }

  return title;
}

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
      if (std::isdigit(static_cast<unsigned char>(*s1)) && std::isdigit(static_cast<unsigned char>(*s2))) {
        // Skip leading zeros
        while (*s1 == '0') s1++;
        while (*s2 == '0') s2++;

        // Count digits to compare lengths first
        int len1 = 0, len2 = 0;
        while (std::isdigit(static_cast<unsigned char>(s1[len1]))) len1++;
        while (std::isdigit(static_cast<unsigned char>(s2[len2]))) len2++;

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
        char c1 = static_cast<char>(std::tolower(static_cast<unsigned char>(*s1)));
        char c2 = static_cast<char>(std::tolower(static_cast<unsigned char>(*s2)));
        if (c1 != c2) return c1 < c2;
        s1++;
        s2++;
      }
    }

    // One string is prefix of other
    return *s1 == '\0' && *s2 != '\0';
  });
}
}  // namespace

void MyLibraryActivity::loadFiles() {
  SpiBusMutex::Guard guard;
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
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(name) + "/");
    } else {
      auto filename = std::string(name);
      if (core::FeatureModules::isSupportedLibraryFile(filename)) {
        files.emplace_back(filename);
      }
    }
    file.close();
  }
  root.close();
  sortFileList(files);
}

void MyLibraryActivity::loadRecentBooks() {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(books.size());
  for (const auto& book : books) {
    if (!Storage.exists(book.path.c_str())) {
      continue;
    }
    auto normalized = book;
    if (normalized.title.empty()) {
      normalized.title = fallbackTitleFromPath(normalized.path);
    }
    recentBooks.push_back(std::move(normalized));
  }
}

size_t MyLibraryActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++) {
    if (files[i] == name) return i;
  }
  return 0;
}

int MyLibraryActivity::getPageItems() const {
  auto metrics = UITheme::getInstance().getMetrics();
  const int contentHeight = renderer.getScreenHeight() - metrics.topPadding - metrics.headerHeight -
                            metrics.tabBarHeight - metrics.verticalSpacing - metrics.buttonHintsHeight -
                            metrics.verticalSpacing;
  const int rowHeight = (currentTab == Tab::Recent) ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  return std::max(1, contentHeight / rowHeight);
}

void MyLibraryActivity::onEnter() {
  Activity::onEnter();

  std::string restoreRecentPath;
  std::string restoreFileName;

  if (currentTab == Tab::Recent) {
    if (!basepath.empty() && basepath != "/") {
      restoreRecentPath = basepath;
    }
    basepath = "/";
  } else if (currentTab == Tab::Files && basepath != "/" && !basepath.empty() && basepath.back() != '/') {
    const auto slash = basepath.find_last_of('/');
    if (slash != std::string::npos) {
      restoreFileName = basepath.substr(slash + 1);
      basepath = (slash == 0) ? "/" : basepath.substr(0, slash);
    }
  }

  loadRecentBooks();
  loadFiles();

  selectorIndex = 0;
  if (currentTab == Tab::Recent && !restoreRecentPath.empty()) {
    for (size_t i = 0; i < recentBooks.size(); ++i) {
      if (recentBooks[i].path == restoreRecentPath) {
        selectorIndex = i;
        break;
      }
    }
  } else if (currentTab == Tab::Files && !restoreFileName.empty()) {
    selectorIndex = findEntry(restoreFileName);
  }

  requestUpdate();
}

void MyLibraryActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  files.clear();
}

void MyLibraryActivity::clearFileMetadata(const std::string& fullPath) {
  // Only clear cache for .epub files
  if (StringUtils::checkFileExtension(fullPath, ".epub")) {
    Epub(fullPath, "/.crosspoint").clearCache();
    LOG_DBG("MyLibrary", "Cleared metadata cache for: %s", fullPath.c_str());
  }
}

void MyLibraryActivity::loop() {
  const int itemCount = getCurrentItemCount();
  const int pageItems = getPageItems();

  if (currentTab == Tab::Recent) {
    // Confirm button - open selected item
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!recentBooks.empty() && selectorIndex < static_cast<int>(recentBooks.size())) {
        onSelectBook(recentBooks[selectorIndex].path);
      }
      return;
    }

    // Back button - go home
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
      return;
    }

    // Tab switching
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      currentTab = Tab::Files;
      selectorIndex = 0;
      requestUpdate();
      return;
    }
  } else {
    // Files tab
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (files.empty()) return;

      const std::string& entry = files[selectorIndex];
      bool isDirectory = (entry.back() == '/');

      if (mappedInput.getHeldTime() >= GO_HOME_MS && !isDirectory) {
        // --- LONG PRESS ACTION: DELETE FILE ---
        std::string cleanBasePath = basepath;
        if (cleanBasePath.back() != '/') cleanBasePath += "/";
        const std::string fullPath = cleanBasePath + entry;

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

    // Back button
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (mappedInput.getHeldTime() < GO_HOME_MS) {
        if (basepath != "/") {
          // Go up one directory, remembering the directory we came from
          const std::string oldPath = basepath;
          basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
          if (basepath.empty()) basepath = "/";
          loadFiles();

          // Select the directory we just came from
          const auto pos = oldPath.find_last_of('/');
          const std::string dirName = oldPath.substr(pos + 1) + "/";
          selectorIndex = static_cast<int>(findEntry(dirName));

          requestUpdate();
        } else {
          onGoHome();
        }
      }
      return;
    }

    // Tab switching
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      currentTab = Tab::Recent;
      selectorIndex = 0;
      requestUpdate();
      return;
    }
  }

  if (core::FeatureModules::hasCapability(core::Capability::VisualCoverPicker) && mappedInput.getHeldTime() >= 500 &&
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    viewMode = (viewMode == ViewMode::List) ? ViewMode::Grid : ViewMode::List;
    requestUpdate();
    return;
  }

  int listSize = static_cast<int>(itemCount);

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
    return filename.substr(0, filename.length() - 1);
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

void MyLibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto folderName = basepath == "/" ? tr(STR_SD_CARD) : basepath.substr(basepath.rfind('/') + 1).c_str();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName);

  std::vector<TabInfo> tabs = {
      {"Recent", currentTab == Tab::Recent},
      {"Files", currentTab == Tab::Files},
  };
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                 false);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (getCurrentItemCount() == 0) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_FILES_FOUND));
  } else {
    if (core::FeatureModules::hasCapability(core::Capability::VisualCoverPicker) && viewMode == ViewMode::Grid) {
      renderGrid();
    } else {
      if (currentTab == Tab::Recent) {
        renderRecentTab(contentTop, contentHeight);
      } else {
        renderFilesTab(contentTop, contentHeight);
      }
    }
  }

  // Help text
  const auto labels = mappedInput.mapLabels(basepath == "/" ? tr(STR_HOME) : tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void MyLibraryActivity::renderRecentTab(int contentTop, int contentHeight) const {
  const auto pageWidth = renderer.getScreenWidth();
  const int bookCount = static_cast<int>(recentBooks.size());

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, bookCount, selectorIndex,
      [this](int index) {
        const auto& book = recentBooks[index];
        if (!book.title.empty()) return book.title;
        std::string title = book.path;
        const size_t lastSlash = title.find_last_of('/');
        if (lastSlash != std::string::npos) title = title.substr(lastSlash + 1);
        const size_t dot = title.find_last_of('.');
        if (dot != std::string::npos) title.resize(dot);
        return title;
      },
      [this](int index) { return recentBooks[index].author; },
      [this](int index) { return UITheme::getFileIcon(recentBooks[index].path); });
}

void MyLibraryActivity::renderFilesTab(int contentTop, int contentHeight) const {
  const auto pageWidth = renderer.getScreenWidth();
  const int fileCount = static_cast<int>(files.size());

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, fileCount, selectorIndex,
      [this](int index) { return files[index]; }, nullptr,
      [this](int index) { return UITheme::getFileIcon(files[index]); });
}

MyLibraryActivity::GridMetrics MyLibraryActivity::getGridMetrics() const {
  const int pageWidth = renderer.getScreenWidth();
  auto metrics = UITheme::getInstance().getMetrics();
  GridMetrics m;
  m.cols = 3;
  m.rows = 2;
  m.paddingX = 20;
  m.paddingY = 20;
  m.startX = 30;
  m.startY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing + 10;
  const int availableWidth = pageWidth - m.startX * 2;
  m.thumbWidth = (availableWidth - (m.cols - 1) * m.paddingX) / m.cols;
  m.thumbHeight = (m.thumbWidth * 3) / 2;  // 2:3 aspect ratio
  return m;
}

void MyLibraryActivity::renderGrid() const {
  const auto m = getGridMetrics();
  const int itemsPerPage = m.cols * m.rows;
  const int itemCount = getCurrentItemCount();
  const int pageStartIndex = selectorIndex / itemsPerPage * itemsPerPage;

  for (int i = 0; i < itemsPerPage && (pageStartIndex + i) < itemCount; i++) {
    const int idx = pageStartIndex + i;
    const int col = i % m.cols;
    const int row = i / m.cols;
    const int x = m.startX + col * (m.thumbWidth + m.paddingX);
    const int y = m.startY + row * (m.thumbHeight + m.paddingY + 20);  // Extra for title
    const bool selected = (idx == selectorIndex);

    std::string path;
    std::string title;
    if (currentTab == Tab::Recent) {
      path = recentBooks[idx].path;
      title = recentBooks[idx].title;
    } else {
      path = basepath + files[idx];
      title = files[idx];
      if (title.back() == '/') title.pop_back();
    }

    if (selected) {
      renderer.drawRect(x - 4, y - 4, m.thumbWidth + 8, m.thumbHeight + 8);
    }

    bool hasCover = false;
    if (path.find(".epub") != std::string::npos || path.find(".xtc") != std::string::npos) {
      hasCover = drawCoverAt(path, x, y, m.thumbWidth, m.thumbHeight);
    }

    if (!hasCover) {
      renderer.drawRect(x, y, m.thumbWidth, m.thumbHeight);
      renderer.drawCenteredText(SMALL_FONT_ID, y + m.thumbHeight / 2 - 4, "No Cover");
    }

    auto truncatedTitle = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), m.thumbWidth);
    renderer.drawText(SMALL_FONT_ID,
                      x + (m.thumbWidth - renderer.getTextWidth(SMALL_FONT_ID, truncatedTitle.c_str())) / 2,
                      y + m.thumbHeight + 5, truncatedTitle.c_str());
  }
}

bool MyLibraryActivity::drawCoverAt(const std::string& path, const int x, const int y, const int width,
                                    const int height) const {
  std::string cacheKey = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(path));
  std::string thumbPath = cacheKey + "/thumb_" + std::to_string(height) + ".bmp";

  if (!Storage.exists(thumbPath.c_str())) {
    return false;
  }

  SpiBusMutex::Guard guard;
  FsFile file;
  if (!Storage.openFileForRead("LIB", thumbPath, file)) {
    return false;
  }

  Bitmap bitmap(file);
  const bool ok = bitmap.parseHeaders() == BmpReaderError::Ok;
  if (ok) {
    renderer.drawBitmap(bitmap, x, y, width, height);
  }
  file.close();
  return ok;
}
