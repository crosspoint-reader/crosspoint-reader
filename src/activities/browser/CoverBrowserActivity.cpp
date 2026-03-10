#include "CoverBrowserActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"

// Reuse the natural-sort from FileBrowserActivity (defined there, not static)
extern void sortFileList(std::vector<std::string>& strs);

void CoverBrowserActivity::loadFiles() {
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
    if (name[0] == '.') {
      file.close();
      continue;
    }
    if (!file.isDirectory()) {
      std::string_view filename{name};
      if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename)) {
        files.emplace_back(filename);
      }
    }
    file.close();
  }
  root.close();
  sortFileList(files);
}

std::string CoverBrowserActivity::getServerThumbPath(const std::string& epubFullPath) const {
  // Server precomputed: {dir}/.covers/{filename}_200.bmp
  auto lastSlash = epubFullPath.rfind('/');
  if (lastSlash == std::string::npos) return {};
  std::string dir = epubFullPath.substr(0, lastSlash);
  std::string filename = epubFullPath.substr(lastSlash + 1);
  char buf[512];
  snprintf(buf, sizeof(buf), "%s/.covers/%s_%d.bmp", dir.c_str(), filename.c_str(), COVER_THUMB_H);
  return buf;
}

std::string CoverBrowserActivity::findThumbPath(const std::string& epubFullPath) const {
  // 1. Server precomputed thumb
  std::string serverPath = getServerThumbPath(epubFullPath);
  if (!serverPath.empty() && Storage.exists(serverPath.c_str())) {
    return serverPath;
  }

  // 2. Device-cached thumb
  Epub epub(epubFullPath, "/.crosspoint");
  std::string devicePath = epub.getThumbBmpPath(COVER_THUMB_H);
  if (Storage.exists(devicePath.c_str())) {
    return devicePath;
  }

  // 3. Generate on device (slow but cached for next time)
  if (epub.load(false, true) && epub.generateThumbBmp(COVER_THUMB_H)) {
    return epub.getThumbBmpPath(COVER_THUMB_H);
  }

  return {};
}

int CoverBrowserActivity::totalPages() const {
  if (files.empty()) return 1;
  return (static_cast<int>(files.size()) + GRID_SIZE - 1) / GRID_SIZE;
}

int CoverBrowserActivity::currentPage() const { return pageOffset / GRID_SIZE; }

void CoverBrowserActivity::onEnter() {
  Activity::onEnter();
  loadFiles();
  selectorIndex = 0;
  pageOffset = 0;
  coverRendered = false;
  coverBufferStored = false;
  bufferRestored = false;
  needFullRefresh = true;
  requestUpdate();
}

void CoverBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
}

void CoverBrowserActivity::loop() {
  const int fileCount = static_cast<int>(files.size());
  if (fileCount == 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  bool selectionChanged = false;
  bool pageChanged = false;

  // Confirm: open selected book
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    int absIndex = pageOffset + selectorIndex;
    if (absIndex < fileCount) {
      std::string fullPath = basepath;
      if (fullPath.back() != '/') fullPath += "/";
      fullPath += files[absIndex];
      onSelectBook(fullPath);
    }
    return;
  }

  // Back: exit cover browser
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Right: next cell
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    int itemsOnPage = std::min(GRID_SIZE, fileCount - pageOffset);
    if (selectorIndex + 1 < itemsOnPage) {
      selectorIndex++;
      selectionChanged = true;
    }
  }

  // Left: previous cell
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (selectorIndex > 0) {
      selectorIndex--;
      selectionChanged = true;
    }
  }

  // Down: next row or next page
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    int itemsOnPage = std::min(GRID_SIZE, fileCount - pageOffset);
    int nextIdx = selectorIndex + GRID_COLS;
    if (nextIdx < itemsOnPage) {
      selectorIndex = nextIdx;
      selectionChanged = true;
    } else if (pageOffset + GRID_SIZE < fileCount) {
      // Next page
      pageOffset += GRID_SIZE;
      int newPageItems = std::min(GRID_SIZE, fileCount - pageOffset);
      int col = selectorIndex % GRID_COLS;
      selectorIndex = std::min(col, newPageItems - 1);
      pageChanged = true;
    }
  }

  // Up: previous row or previous page
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    int prevIdx = selectorIndex - GRID_COLS;
    if (prevIdx >= 0) {
      selectorIndex = prevIdx;
      selectionChanged = true;
    } else if (pageOffset > 0) {
      // Previous page
      pageOffset -= GRID_SIZE;
      int col = selectorIndex % GRID_COLS;
      // Land on last row, same column
      int newPageItems = std::min(GRID_SIZE, fileCount - pageOffset);
      int lastRow = (newPageItems - 1) / GRID_COLS;
      selectorIndex = std::min(lastRow * GRID_COLS + col, newPageItems - 1);
      pageChanged = true;
    }
  }

  // Page forward/back via side buttons
  if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    if (pageOffset + GRID_SIZE < fileCount) {
      pageOffset += GRID_SIZE;
      int newPageItems = std::min(GRID_SIZE, fileCount - pageOffset);
      selectorIndex = std::min(selectorIndex, newPageItems - 1);
      pageChanged = true;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    if (pageOffset > 0) {
      pageOffset -= GRID_SIZE;
      int newPageItems = std::min(GRID_SIZE, fileCount - pageOffset);
      selectorIndex = std::min(selectorIndex, newPageItems - 1);
      pageChanged = true;
    }
  }

  if (pageChanged) {
    coverRendered = false;
    coverBufferStored = false;
    bufferRestored = false;
    needFullRefresh = true;
    requestUpdate();
  } else if (selectionChanged) {
    needFullRefresh = false;
    requestUpdate();
  }
}

void CoverBrowserActivity::drawCoverCell(int cellIndex, int gridX, int gridY, int /*cellW*/, int /*cellH*/) {
  int absIndex = pageOffset + cellIndex;
  if (absIndex >= static_cast<int>(files.size())) return;

  // Stamp cover at native COVER_THUMB_W x COVER_THUMB_H — no scaling
  int coverX = gridX + CELL_PADDING;
  int coverY = gridY + CELL_PADDING;

  std::string fullPath = basepath;
  if (fullPath.back() != '/') fullPath += "/";
  fullPath += files[absIndex];

  std::string thumbPath = findThumbPath(fullPath);
  bool hasCover = false;

  if (!thumbPath.empty()) {
    FsFile file;
    if (Storage.openFileForRead("COV", thumbPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        renderer.drawBitmap(bitmap, coverX, coverY, COVER_THUMB_W, COVER_THUMB_H);
        hasCover = true;
      }
      file.close();
    }
  }

  // Draw border around cover area
  renderer.drawRect(coverX, coverY, COVER_THUMB_W, COVER_THUMB_H, true);

  if (!hasCover) {
    // Placeholder: fill lower 2/3 and draw book icon
    renderer.fillRect(coverX, coverY + COVER_THUMB_H / 3, COVER_THUMB_W, COVER_THUMB_H * 2 / 3, true);
    renderer.drawIcon(CoverIcon, coverX + (COVER_THUMB_W - 32) / 2, coverY + (COVER_THUMB_H - 32) / 2, 32, 32);
  }

  // Title below cover (single line, truncated to cover width)
  std::string filename = files[absIndex];
  auto dotPos = filename.rfind('.');
  if (dotPos != std::string::npos) filename = filename.substr(0, dotPos);

  int titleY = coverY + COVER_THUMB_H + 4;
  auto titleLines = renderer.wrappedText(SMALL_FONT_ID, filename.c_str(), COVER_THUMB_W, 1);
  for (const auto& line : titleLines) {
    renderer.drawText(SMALL_FONT_ID, coverX, titleY, line.c_str(), true);
  }
}

void CoverBrowserActivity::drawSelectionHighlight(int cellIndex, int gridX, int gridY) {
  if (cellIndex != selectorIndex) return;

  // Draw 2px border rect around the fixed-size cell
  for (int i = 0; i < SELECTION_BORDER; i++) {
    renderer.drawRect(gridX + i, gridY + i, CELL_W - 2 * i, CELL_H - 2 * i, true);
  }
}

void CoverBrowserActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Fixed grid dimensions: 3x3 cells of CELL_W x CELL_H, centered in content area
  static constexpr int GRID_W = GRID_COLS * CELL_W;   // 417
  static constexpr int GRID_H = GRID_ROWS * CELL_H;   // 663

  int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  int availableHeight = contentBottom - contentTop;

  // Center grid in available area
  int gridOriginX = (pageWidth - GRID_W) / 2;
  int gridOriginY = contentTop + (availableHeight - GRID_H) / 2;

  int itemsOnPage = std::min(GRID_SIZE, static_cast<int>(files.size()) - pageOffset);

  auto cellPos = [&](int i, int& gx, int& gy) {
    gx = gridOriginX + (i % GRID_COLS) * CELL_W;
    gy = gridOriginY + (i / GRID_COLS) * CELL_H;
  };

  if (needFullRefresh || !coverBufferStored) {
    renderer.clearScreen();

    // Header with page indicator
    std::string folderName = (basepath == "/") ? tr(STR_SD_CARD) : basepath.substr(basepath.rfind('/') + 1);
    char headerBuf[128];
    snprintf(headerBuf, sizeof(headerBuf), "%s (%d/%d)", folderName.c_str(), currentPage() + 1, totalPages());
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerBuf);

    if (files.empty()) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_FILES_FOUND));
    } else if (!coverRendered) {
      // Render all covers at native resolution (slow — SD reads)
      for (int i = 0; i < itemsOnPage; i++) {
        int gx, gy;
        cellPos(i, gx, gy);
        drawCoverCell(i, gx, gy, CELL_W, CELL_H);
      }

      // Store framebuffer for fast selection redraws
      coverBufferStored = renderer.storeBwBuffer();
      coverRendered = coverBufferStored;
    }

    // Draw selection highlight
    if (coverRendered) {
      for (int i = 0; i < itemsOnPage; i++) {
        int gx, gy;
        cellPos(i, gx, gy);
        drawSelectionHighlight(i, gx, gy);
      }
    }

    // Button hints
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    renderer.displayBuffer(needFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
    needFullRefresh = false;
  } else {
    // Fast path: restore stored cover buffer, redraw only selection
    renderer.restoreBwBuffer();

    for (int i = 0; i < itemsOnPage; i++) {
      int gx, gy;
      cellPos(i, gx, gy);
      drawSelectionHighlight(i, gx, gy);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}
