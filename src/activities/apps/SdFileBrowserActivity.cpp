#include "SdFileBrowserActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void SdFileBrowserActivity::onEnter() {
  Activity::onEnter();
  currentPath = "/";
  loadDirectory();
  requestUpdate();
}

void SdFileBrowserActivity::onExit() {
  entries.clear();
  viewContent.clear();
  Activity::onExit();
}

void SdFileBrowserActivity::loadDirectory() {
  entries.clear();
  selectorIndex = 0;

  HalFile dir = Storage.open(currentPath.c_str());
  if (!dir) return;

  // Add ".." if not root
  if (currentPath != "/") {
    entries.push_back({"..", true, 0});
  }

  HalFile entry;
  while ((entry = dir.openNextFile()) && entries.size() < 200) {
    char nameBuf[256];
    entry.getName(nameBuf, sizeof(nameBuf));
    std::string name = nameBuf;

    // Skip hidden files and system dirs
    if (name.empty() || name[0] == '.' || name == "System Volume Information") {
      entry.close();
      continue;
    }

    FileEntry fe;
    fe.name = name;
    fe.isDir = entry.isDirectory();
    fe.size = fe.isDir ? 0 : entry.size();
    entries.push_back(std::move(fe));
    entry.close();
  }
  dir.close();

  // Sort: directories first (alpha), then files (alpha)
  std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
    if (a.name == "..") return true;
    if (b.name == "..") return false;
    if (a.isDir != b.isDir) return a.isDir > b.isDir;
    return a.name < b.name;
  });
}

void SdFileBrowserActivity::navigateInto(int index) {
  if (entries[index].name == "..") {
    navigateUp();
    return;
  }
  if (currentPath == "/") {
    currentPath = "/" + entries[index].name;
  } else {
    currentPath = currentPath + "/" + entries[index].name;
  }
  loadDirectory();
  requestUpdate();
}

void SdFileBrowserActivity::navigateUp() {
  if (currentPath == "/") {
    finish();
    return;
  }
  // Store current dir name to find it after navigating up
  size_t lastSlash = currentPath.rfind('/');
  std::string previousDir;
  if (lastSlash != std::string::npos && lastSlash > 0) {
    previousDir = currentPath.substr(lastSlash + 1);
    currentPath = currentPath.substr(0, lastSlash);
  } else {
    currentPath = "/";
  }
  loadDirectory();
  // Try to select the directory we came from
  for (int i = 0; i < static_cast<int>(entries.size()); i++) {
    if (entries[i].name == previousDir) {
      selectorIndex = i;
      break;
    }
  }
  requestUpdate();
}

void SdFileBrowserActivity::viewFile(int index) {
  const auto& e = entries[index];
  std::string fullPath;
  if (currentPath == "/") {
    fullPath = "/" + e.name;
  } else {
    fullPath = currentPath + "/" + e.name;
  }

  // Read up to 16KB
  static constexpr size_t MAX_VIEW_BYTES = 16384;
  char buffer[1024];  // Read in chunks to avoid large stack allocation
  viewContent.clear();
  viewContent.reserve(std::min(static_cast<uint32_t>(MAX_VIEW_BYTES), e.size));

  HalFile file = Storage.open(fullPath.c_str());
  if (!file) return;

  size_t totalRead = 0;
  while (totalRead < MAX_VIEW_BYTES) {
    size_t toRead = std::min(sizeof(buffer), MAX_VIEW_BYTES - totalRead);
    int bytesRead = file.read(buffer, toRead);
    if (bytesRead <= 0) break;
    viewContent.append(buffer, bytesRead);
    totalRead += bytesRead;
  }
  file.close();

  if (totalRead >= MAX_VIEW_BYTES) {
    viewContent += "\n\n[Truncated at 16KB]";
  }

  viewScrollOffset = 0;
  state = VIEWING_FILE;
  requestUpdate();
}

void SdFileBrowserActivity::showFileInfo(int index) {
  (void)index;
  state = FILE_INFO;
  requestUpdate();
}

void SdFileBrowserActivity::deleteFile() {
  Storage.remove(deleteTargetPath.c_str());
  loadDirectory();
  state = BROWSING;
  requestUpdate();
}

std::string SdFileBrowserActivity::formatSize(uint32_t bytes) {
  if (bytes < 1024) return std::to_string(bytes) + " B";
  if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
  return std::to_string(bytes / (1024 * 1024)) + " MB";
}

bool SdFileBrowserActivity::isTextFile(const std::string& name) {
  size_t dotPos = name.rfind('.');
  if (dotPos == std::string::npos) return false;

  std::string ext = name.substr(dotPos);
  // Convert to lowercase
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z') c = c + ('a' - 'A');
  }

  static const char* const textExts[] = {
      ".txt", ".csv",  ".json", ".html", ".htm",  ".md",   ".log",
      ".xml", ".ini",  ".cfg",  ".yml",  ".yaml", ".toml", ".conf",
      ".sh",  ".bat",  ".py",   ".js",   ".css",
  };
  for (const char* e : textExts) {
    if (ext == e) return true;
  }
  return false;
}

void SdFileBrowserActivity::loop() {
  if (state == BROWSING) {
    const int count = static_cast<int>(entries.size());

    buttonNavigator.onNext([this, count] {
      if (count > 0) {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, count);
        requestUpdate();
      }
    });

    buttonNavigator.onPrevious([this, count] {
      if (count > 0) {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, count);
        requestUpdate();
      }
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectorIndex < count) {
        const auto& e = entries[selectorIndex];
        if (e.isDir) {
          navigateInto(selectorIndex);
        } else if (isTextFile(e.name)) {
          viewFile(selectorIndex);
        } else {
          showFileInfo(selectorIndex);
        }
      }
    }

    // PageForward side button: show file info
    if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
      if (selectorIndex < count && entries[selectorIndex].name != "..") {
        showFileInfo(selectorIndex);
      }
    }

    // PageBack side button: delete (only files, not ".." or directories)
    if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
      if (selectorIndex < count) {
        const auto& e = entries[selectorIndex];
        if (e.name != ".." && !e.isDir) {
          deleteTargetName = e.name;
          if (currentPath == "/") {
            deleteTargetPath = "/" + e.name;
          } else {
            deleteTargetPath = currentPath + "/" + e.name;
          }
          state = CONFIRM_DELETE;
          requestUpdate();
        }
      }
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      if (currentPath == "/") {
        finish();
      } else {
        navigateUp();
      }
    }
    return;
  }

  if (state == VIEWING_FILE) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    const int pageHeight = renderer.getScreenHeight();
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    int visibleLines = contentHeight / lineH;
    if (visibleLines < 1) visibleLines = 1;

    buttonNavigator.onNext([this, visibleLines] {
      if (viewScrollOffset + visibleLines < viewTotalLines) {
        viewScrollOffset += visibleLines;
        requestUpdate();
      }
    });

    buttonNavigator.onPrevious([this, visibleLines] {
      if (viewScrollOffset > 0) {
        viewScrollOffset -= visibleLines;
        if (viewScrollOffset < 0) viewScrollOffset = 0;
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = BROWSING;
      viewContent.clear();
      requestUpdate();
    }
    return;
  }

  if (state == FILE_INFO) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = BROWSING;
      requestUpdate();
    }
    return;
  }

  if (state == CONFIRM_DELETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      deleteFile();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = BROWSING;
      requestUpdate();
    }
    return;
  }
}

void SdFileBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (state == BROWSING) {
    // Header with current path (truncated if too long)
    std::string headerTitle = currentPath;
    if (headerTitle.length() > 30) {
      headerTitle = "..." + headerTitle.substr(headerTitle.length() - 27);
    }
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerTitle.c_str());

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    if (entries.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Empty directory");
    } else {
      GUI.drawList(
          renderer, Rect{0, contentTop, pageWidth, contentHeight},
          static_cast<int>(entries.size()), selectorIndex,
          [this](int index) -> std::string {
            const auto& e = entries[index];
            return e.isDir ? (e.name + "/") : e.name;
          },
          [this](int index) -> std::string {
            const auto& e = entries[index];
            if (e.name == "..") return "Parent directory";
            return e.isDir ? "<DIR>" : formatSize(e.size);
          });
    }

    const auto labels = mappedInput.mapLabels("Back", "Open", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    GUI.drawSideButtonHints(renderer, "Info", "Delete");

    renderer.displayBuffer();
    return;
  }

  if (state == VIEWING_FILE) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "File Viewer");

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    const int maxWidth = pageWidth - 2 * metrics.contentSidePadding;

    auto lines = renderer.wrappedText(UI_10_FONT_ID, viewContent.c_str(), maxWidth, 0);
    viewTotalLines = static_cast<int>(lines.size());
    int visibleLines = contentHeight / lineH;
    if (visibleLines < 1) visibleLines = 1;

    int y = contentTop;
    for (int i = viewScrollOffset; i < viewScrollOffset + visibleLines && i < viewTotalLines; i++) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, lines[i].c_str());
      y += lineH;
    }

    // Page indicator
    int totalPages = (viewTotalLines + visibleLines - 1) / visibleLines;
    int currentPage = (viewScrollOffset / visibleLines) + 1;
    char pageInfo[32];
    snprintf(pageInfo, sizeof(pageInfo), "%d/%d", currentPage, totalPages > 0 ? totalPages : 1);
    int pageInfoWidth = renderer.getTextWidth(SMALL_FONT_ID, pageInfo);
    renderer.drawText(SMALL_FONT_ID, pageWidth - pageInfoWidth - metrics.contentSidePadding,
                      pageHeight - metrics.buttonHintsHeight - 20, pageInfo);

    const auto labels = mappedInput.mapLabels("Back", "", "PgUp", "PgDn");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    renderer.displayBuffer();
    return;
  }

  if (state == FILE_INFO) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "File Info");

    const int leftPad = metrics.contentSidePadding;
    int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 20;
    const int lineH = 45;

    if (selectorIndex < static_cast<int>(entries.size())) {
      const auto& e = entries[selectorIndex];
      std::string fullPath = (currentPath == "/") ? ("/" + e.name) : (currentPath + "/" + e.name);

      renderer.drawText(SMALL_FONT_ID, leftPad, y, "Name", true, EpdFontFamily::BOLD);
      y += 22;
      renderer.drawText(UI_10_FONT_ID, leftPad, y, e.name.c_str());
      y += lineH;

      renderer.drawText(SMALL_FONT_ID, leftPad, y, "Path", true, EpdFontFamily::BOLD);
      y += 22;
      renderer.drawText(UI_10_FONT_ID, leftPad, y, fullPath.c_str());
      y += lineH;

      renderer.drawText(SMALL_FONT_ID, leftPad, y, "Type", true, EpdFontFamily::BOLD);
      y += 22;
      renderer.drawText(UI_10_FONT_ID, leftPad, y, e.isDir ? "Directory" : "File");
      y += lineH;

      if (!e.isDir) {
        renderer.drawText(SMALL_FONT_ID, leftPad, y, "Size", true, EpdFontFamily::BOLD);
        y += 22;
        renderer.drawText(UI_10_FONT_ID, leftPad, y, formatSize(e.size).c_str());
      }
    }

    const auto labels = mappedInput.mapLabels("Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == CONFIRM_DELETE) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Confirm Delete");

    int y = pageHeight / 2 - 30;
    std::string msg = "Delete \"" + deleteTargetName + "\"?";
    renderer.drawCenteredText(UI_12_FONT_ID, y, msg.c_str(), true, EpdFontFamily::BOLD);
    y += 40;
    renderer.drawCenteredText(UI_10_FONT_ID, y, deleteTargetPath.c_str());

    const auto labels = mappedInput.mapLabels("Cancel", "Delete", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}
