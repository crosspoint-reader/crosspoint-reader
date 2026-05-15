#include "RecentBooksActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <Xtc.h>

#include <algorithm>
#include <string>

#include "../ActivityManager.h"
#include "../util/ConfirmationActivity.h"
#include "BookInfoActivity.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

namespace {
std::string convertSidecarToBmp(const std::string& bookPath, const std::string& sidecarPath, int width, int height,
                                const std::string& fileName) {
  const std::string cacheDir = "/.crosspoint/sidecar_" + std::to_string(std::hash<std::string>{}(bookPath));
  Storage.mkdir(cacheDir.c_str());
  const std::string bmpPath = cacheDir + "/" + fileName;
  if (Storage.exists(bmpPath.c_str())) return bmpPath;

  FsFile src;
  if (!Storage.openFileForRead("RBA", sidecarPath, src)) return "";
  FsFile dst;
  if (!Storage.openFileForWrite("RBA", bmpPath, dst)) {
    src.close();
    return "";
  }

  bool ok = false;
  if (FsHelpers::hasJpgExtension(sidecarPath)) {
    ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(src, dst, width, height);
  } else if (FsHelpers::hasPngExtension(sidecarPath)) {
    ok = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(src, dst, width, height);
  }
  src.close();
  dst.close();
  if (!ok) {
    Storage.remove(bmpPath.c_str());
    return "";
  }
  return bmpPath;
}

int gridThumbWidth(int contentWidth) {
  const int margin = RecentBooksActivity::GRID_THUMB_MARGIN;
  const int cols = RecentBooksActivity::GRID_COLS;
  return (contentWidth - (cols + 1) * margin) / cols;
}

std::string gridThumbPath(const std::string& coverBmpPath, int tw, int th) {
  return UITheme::getCoverThumbPath(coverBmpPath, tw, th);
}
}  // namespace

void RecentBooksActivity::loadRecentBooks() { recentBooks = RECENT_BOOKS.getBooks(); }

bool RecentBooksActivity::loadNextCover() {
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);
  const int tw = gridThumbWidth(contentRect.width);
  const int th = GRID_THUMB_HEIGHT;

  for (; nextCoverIndex < recentBooks.size(); nextCoverIndex++) {
    RecentBook& book = recentBooks[nextCoverIndex];
    if (book.coverBmpPath.empty()) continue;

    const bool isSidecar =
        FsHelpers::hasJpgExtension(book.coverBmpPath) || FsHelpers::hasPngExtension(book.coverBmpPath);

    if (isSidecar) {
      if (!Storage.exists(book.coverBmpPath.c_str())) {
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, "");
        book.coverBmpPath = "";
        continue;
      }
      const std::string cacheBase = "/.crosspoint/sidecar_" + std::to_string(std::hash<std::string>{}(book.path));
      const std::string placeholder = cacheBase + "/[HEIGHT].bmp";
      const std::string name = std::to_string(tw) + "x" + std::to_string(th) + ".bmp";
      const std::string result = convertSidecarToBmp(book.path, book.coverBmpPath, tw, th, name);
      if (!result.empty()) {
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, placeholder);
        book.coverBmpPath = placeholder;
      }
      nextCoverIndex++;
      return false;
    }

    const std::string thumbPath = gridThumbPath(book.coverBmpPath, tw, th);
    if (!Storage.exists(thumbPath.c_str())) {
      bool ok = false;
      if (FsHelpers::hasEpubExtension(book.path)) {
        Epub epub(book.path, "/.crosspoint");
        epub.load(false, true);
        ok = epub.generateThumbBmp(tw, th);
      } else if (FsHelpers::hasXtcExtension(book.path)) {
        Xtc xtc(book.path, "/.crosspoint");
        if (xtc.load()) ok = xtc.generateThumbBmp(tw, th);
      }
      if (!ok) {
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, "");
        book.coverBmpPath = "";
      }
      nextCoverIndex++;
      return false;
    }
  }

  return true;
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  loadRecentBooks();

  selectorIndex = 0;
  if (initialFocusIndex >= 0 && initialFocusIndex < static_cast<int>(recentBooks.size())) {
    selectorIndex = initialFocusIndex;
  }
  initialFocusIndex = -1;

  coversLoaded = false;
  coversLoading = false;
  firstRenderDone = false;
  nextCoverIndex = 0;
  prevSelectorIndex = -1;
  fullRedrawNeeded = true;

  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
}

void RecentBooksActivity::switchViewMode(bool grid) {
  APP_STATE.recentBooksGridView = grid;
  APP_STATE.saveToFile();
  coversLoaded = false;
  coversLoading = false;
  firstRenderDone = false;
  nextCoverIndex = 0;
  prevSelectorIndex = -1;
  fullRedrawNeeded = true;
  requestUpdate(true);
}

void RecentBooksActivity::removeSelectedBook() {
  if (recentBooks.empty() || selectorIndex >= static_cast<int>(recentBooks.size())) return;
  const std::string bookPath = recentBooks[selectorIndex].path;
  const std::string bookTitle = recentBooks[selectorIndex].title;
  auto handler = [this, bookPath](const ActivityResult& res) {
    if (!res.isCancelled) {
      LOG_DBG("RBA", "Removing from recent books: %s", bookPath.c_str());
      RECENT_BOOKS.removeBook(bookPath);
      loadRecentBooks();
      if (recentBooks.empty()) {
        selectorIndex = 0;
      } else if (selectorIndex >= static_cast<int>(recentBooks.size())) {
        selectorIndex = static_cast<int>(recentBooks.size()) - 1;
      }
      prevSelectorIndex = -1;
      fullRedrawNeeded = true;
      requestUpdate(true);
    }
  };
  std::string heading = tr(STR_REMOVE) + std::string("? ");
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, bookTitle), handler);
}

void RecentBooksActivity::showSelectedBookInfo() {
  if (recentBooks.empty() || selectorIndex >= static_cast<int>(recentBooks.size())) return;
  const std::string& path = recentBooks[selectorIndex].path;
  if (FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path)) {
    startActivityForResult(std::make_unique<BookInfoActivity>(renderer, mappedInput, path),
                           [this](const ActivityResult&) { requestUpdate(); });
  }
}

void RecentBooksActivity::loop() {
  const bool gridView = APP_STATE.recentBooksGridView;
  const int listSize = static_cast<int>(recentBooks.size());

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    // Confirm short/long: open book (long = KOReader sync for EPUBs)
    if (ev.button == MappedInputManager::Button::Confirm &&
        (ev.type == ButtonEventManager::PressType::Short || ev.type == ButtonEventManager::PressType::Long)) {
      if (recentBooks.empty() || selectorIndex >= static_cast<int>(recentBooks.size())) return;
      const bool longPress = (ev.type == ButtonEventManager::PressType::Long) && KOREADER_STORE.hasCredentials();
      const std::string& selectedPath = recentBooks[selectorIndex].path;
      const bool isEpubBook = FsHelpers::hasEpubExtension(selectedPath);
      LOG_DBG("RBA", "Selected recent book: %s (sync=%d epub=%d)", selectedPath.c_str(), longPress ? 1 : 0,
              isEpubBook ? 1 : 0);
      if (longPress && isEpubBook) {
        auto& sync = APP_STATE.koReaderSyncSession;
        sync.autoPullEpubPath = selectedPath;
        sync.exitToHomeAfterSync = false;
        APP_STATE.saveToFile();
      }
      ReturnHint hint;
      hint.target = ReturnTo::RecentBooks;
      hint.selectIndex = selectorIndex;
      activityManager.replaceWithReader(recentBooks[selectorIndex].path, std::move(hint));
      return;
    }

    // Back short: go home
    if (ev.button == MappedInputManager::Button::Back && ev.type == ButtonEventManager::PressType::Short) {
      onGoHome();
      return;
    }

    // Up short: navigate (row up in grid, previous in list)
    if (ev.button == MappedInputManager::Button::Up && ev.type == ButtonEventManager::PressType::Short) {
      if (!recentBooks.empty()) {
        if (gridView) {
          selectorIndex = std::max(0, selectorIndex - GRID_COLS);
        } else {
          selectorIndex = ButtonNavigator::previousIndex(selectorIndex, listSize);
        }
        requestUpdate();
      }
      continue;
    }

    // Down short: navigate (row down in grid, next in list)
    if (ev.button == MappedInputManager::Button::Down && ev.type == ButtonEventManager::PressType::Short) {
      if (!recentBooks.empty()) {
        if (gridView) {
          selectorIndex = std::min(listSize - 1, selectorIndex + GRID_COLS);
        } else {
          selectorIndex = ButtonNavigator::nextIndex(selectorIndex, listSize);
        }
        requestUpdate();
      }
      continue;
    }

    // Up long: toggle between list and grid view
    if (ev.button == MappedInputManager::Button::Up && ev.type == ButtonEventManager::PressType::Long) {
      switchViewMode(!gridView);
      return;
    }

    // Left short: column left in grid, previous in list
    if (ev.button == MappedInputManager::Button::Left && ev.type == ButtonEventManager::PressType::Short) {
      if (!recentBooks.empty()) {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, listSize);
        requestUpdate();
      }
      continue;
    }

    // Right short: column right in grid, next in list
    if (ev.button == MappedInputManager::Button::Right && ev.type == ButtonEventManager::PressType::Short) {
      if (!recentBooks.empty()) {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, listSize);
        requestUpdate();
      }
      continue;
    }

    // Left long: remove selected book (both views)
    if (ev.button == MappedInputManager::Button::Left && ev.type == ButtonEventManager::PressType::Long) {
      removeSelectedBook();
      return;
    }

    // Right long: show book info (both views)
    if (ev.button == MappedInputManager::Button::Right && ev.type == ButtonEventManager::PressType::Long) {
      showSelectedBookInfo();
      return;
    }
  }
}

void RecentBooksActivity::render(RenderLock&& lock) {
  if (APP_STATE.recentBooksGridView) {
    renderGridView(std::move(lock));
    if (!firstRenderDone) {
      firstRenderDone = true;
      requestUpdate();
    } else if (!coversLoaded && !coversLoading) {
      coversLoading = true;
      if (loadNextCover()) {
        coversLoaded = true;
      } else {
        fullRedrawNeeded = true;
        requestUpdate();
      }
      coversLoading = false;
    }
  } else {
    renderListView(std::move(lock));
  }
}

void RecentBooksActivity::renderListView(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_MENU_RECENT_BOOKS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;

  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, contentTop + 20,
                      tr(STR_NO_RECENT_BOOKS));
  } else {
    GUI.drawList(
        renderer, Rect{contentRect.x, contentTop, contentRect.width, contentHeight},
        static_cast<int>(recentBooks.size()), selectorIndex, [this](int index) { return recentBooks[index].title; },
        [this](int index) {
          const auto& book = recentBooks[index];
          if (!book.author.empty() && !book.series.empty()) return book.author + "\n" + book.series;
          if (!book.series.empty()) return book.series;
          return book.author;
        },
        [this](int index) { return UITheme::getFileIcon(recentBooks[index].path); });
  }

  const bool hasBooks = !recentBooks.empty();
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), hasBooks ? tr(STR_OPEN) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  renderer.displayBuffer();
}

void RecentBooksActivity::renderGridCell(int index, bool selected, int cellX, int cellY, int tw, int th, int labelW) {
  const auto& book = recentBooks[index];
  const int labelY = cellY + th + 3;
  const int cellFillHeight = th + GRID_LABEL_HEIGHT + 3;

  if (selected) {
    renderer.fillRect(cellX, cellY, tw, cellFillHeight);
    renderer.drawRect(cellX, cellY, tw, th, false);
  } else {
    // Clear to white before redrawing (needed when deselecting)
    renderer.fillRect(cellX, cellY, tw, cellFillHeight, false);
    renderer.drawRect(cellX, cellY, tw, th);
  }

  if (!book.coverBmpPath.empty()) {
    const std::string thumbPath = gridThumbPath(book.coverBmpPath, tw, th);
    FsFile file;
    if (Storage.openFileForRead("RBA", thumbPath, file)) {
      Bitmap bmp(file);
      if (bmp.parseHeaders() == BmpReaderError::Ok) {
        const int imgW = bmp.getWidth();
        const int imgH = bmp.getHeight();
        const int innerW = tw - 2;
        const int innerH = th - 2;
        if (imgW > 0 && imgH > 0) {
          // Mirror drawBitmap1Bit scale = min(maxW/imgW, maxH/imgH) to get rendered size,
          // then center the image within the frame.
          const float scaleX = static_cast<float>(innerW) / imgW;
          const float scaleY = static_cast<float>(innerH) / imgH;
          // Cap at 1.0: never upscale (drawBitmap1Bit also won't upscale beyond maxW/maxH).
          const float scale = std::min(1.0f, std::min(scaleX, scaleY));
          const int rendW = static_cast<int>(imgW * scale);
          const int rendH = static_cast<int>(imgH * scale);
          const int offsetX = std::max(1, (tw - rendW) / 2);
          const int offsetY = std::max(1, (th - rendH) / 2);
          // Pre-clear only the exact rendered image area; the black selection background
          // shows through in the surrounding space.
          renderer.fillRect(cellX + offsetX, cellY + offsetY, rendW, rendH, false);
          renderer.drawBitmap1Bit(bmp, cellX + offsetX, cellY + offsetY, rendW, rendH);
        }
      }
      file.close();
    }
  } else {
    // No cover — clear the whole interior so the placeholder looks clean.
    renderer.fillRect(cellX + 1, cellY + 1, tw - 2, th - 2, false);
  }

  // Label: title line 1, author line 2; white text on black for selected, black on white otherwise
  const bool black = !selected;
  std::string titleStr = renderer.truncatedText(SMALL_FONT_ID, book.title.c_str(), labelW);
  renderer.drawText(SMALL_FONT_ID, cellX + 2, labelY, titleStr.c_str(), black);
  if (!book.author.empty()) {
    std::string authorStr = renderer.truncatedText(SMALL_FONT_ID, book.author.c_str(), labelW);
    renderer.drawText(SMALL_FONT_ID, cellX + 2, labelY + 17, authorStr.c_str(), black);
  }
}

void RecentBooksActivity::renderGridView(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;
  const int margin = GRID_THUMB_MARGIN;
  const int tw = gridThumbWidth(contentRect.width);
  const int th = GRID_THUMB_HEIGHT;
  const int cellHeight = th + GRID_LABEL_HEIGHT + margin;
  const int visibleRows = std::max(1, contentHeight / cellHeight);
  const int totalRows = (static_cast<int>(recentBooks.size()) + GRID_COLS - 1) / GRID_COLS;
  const int selectedRow = selectorIndex / GRID_COLS;
  const int pageStartRow = (selectedRow / visibleRows) * visibleRows;
  const int startIndex = pageStartRow * GRID_COLS;
  const int labelW = tw - 4;

  auto cellPos = [&](int i, int& cx, int& cy) {
    const int row = (i / GRID_COLS) - pageStartRow;
    const int col = i % GRID_COLS;
    cx = contentRect.x + margin + col * (tw + margin);
    cy = contentTop + row * cellHeight;
  };

  // Partial fast path: only the selection changed within the same page
  const int prevPage = prevSelectorIndex >= 0 ? (prevSelectorIndex / GRID_COLS / visibleRows) : -1;
  const int curPage = selectedRow / visibleRows;
  if (!fullRedrawNeeded && prevSelectorIndex >= 0 && prevSelectorIndex != selectorIndex && prevPage == curPage) {
    int cx, cy;
    cellPos(prevSelectorIndex, cx, cy);
    renderGridCell(prevSelectorIndex, false, cx, cy, tw, th, labelW);
    cellPos(selectorIndex, cx, cy);
    renderGridCell(selectorIndex, true, cx, cy, tw, th, labelW);
    prevSelectorIndex = selectorIndex;
    renderer.displayBuffer();
    return;
  }

  // Full redraw
  fullRedrawNeeded = false;
  prevSelectorIndex = selectorIndex;

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_MENU_RECENT_BOOKS));

  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, contentTop + 20,
                      tr(STR_NO_RECENT_BOOKS));
    const auto labels = mappedInput.mapLabels(tr(STR_HOME), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int endIndex = std::min(startIndex + visibleRows * GRID_COLS, static_cast<int>(recentBooks.size()));
  for (int i = startIndex; i < endIndex; i++) {
    int cx, cy;
    cellPos(i, cx, cy);
    renderGridCell(i, i == selectorIndex, cx, cy, tw, th, labelW);
  }

  // Scroll arrows when content spans multiple pages
  if (totalRows > visibleRows) {
    constexpr int arrowSize = 6;
    const int centerX = contentRect.x + contentRect.width / 2;
    if (pageStartRow > 0) {
      const int arrowY = contentTop + 2;
      for (int j = 0; j < arrowSize; ++j) {
        const int half = arrowSize - 1 - j;
        renderer.drawLine(centerX - half, arrowY + j, centerX + half, arrowY + j);
      }
    }
    if (pageStartRow + visibleRows < totalRows) {
      const int arrowY = contentTop + contentHeight - arrowSize - 2;
      for (int j = 0; j < arrowSize; ++j) {
        renderer.drawLine(centerX - j, arrowY + j, centerX + j, arrowY + j);
      }
    }
  }

  // On X4 (taller screen) there is room for a one-line gesture hint below the grid.
  if (!gpio.deviceIsX3()) {
    const int hintY = contentRect.y + contentRect.height - metrics.verticalSpacing - 14;
    const std::string hint = std::string(tr(STR_DIR_UP)) + "+L: " + tr(STR_VIEW_GRID) + "/" + tr(STR_VIEW_LIST) +
                             "   " + tr(STR_DIR_LEFT) + "+L: " + tr(STR_REMOVE) + "   " + tr(STR_DIR_RIGHT) +
                             "+L: " + tr(STR_INFO);
    renderer.drawText(SMALL_FONT_ID, contentRect.x + metrics.contentSidePadding, hintY, hint.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  renderer.displayBuffer();
}
