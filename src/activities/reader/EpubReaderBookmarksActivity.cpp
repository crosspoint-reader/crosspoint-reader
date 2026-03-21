#include "EpubReaderBookmarksActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
} 

// Layout constants used in renderScreen
const int LINE_HEIGHT = 60;

int EpubReaderBookmarksActivity::getPageItems() const {
  const int screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  // In inverted portrait, the button hints are drawn near the logical top.
  // Reserve vertical space so list items do not collide with the hints.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = 60 + hintGutterHeight;
  const int availableHeight = screenHeight - startY - LINE_HEIGHT;
  // Clamp to at least one item to avoid division by zero and empty paging.
  return std::max(1, availableHeight / LINE_HEIGHT);
}

void EpubReaderBookmarksActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  bookmarks = BookmarkStore::loadBookmarks(epubPath);
  LOG_DBG("EPB", "Loaded %d bookmarks for book: %s", static_cast<int>(bookmarks.size()), epubPath.c_str());

  // Trigger first update
  requestUpdate();
}

void EpubReaderBookmarksActivity::onExit() { Activity::onExit(); }

void EpubReaderBookmarksActivity::loop() {
  const int pageItems = getPageItems();


  // Delete confirmation mode
  if (confirmingDelete) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!BookmarkStore::deleteBookmark(epubPath, selectorIndex)) {
        LOG_DBG("EPB", "Failed to delete bookmark at index %d", selectorIndex);
      } else {
        bookmarks.erase(bookmarks.begin() + selectorIndex);
        // Move selector up if we deleted the last item
        if (selectorIndex >= bookmarks.size() && selectorIndex > 0) {
          selectorIndex--;
        }
      }
      requestUpdate();
      confirmingDelete = false;
      return;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      requestUpdate();
      confirmingDelete = false;
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {  // Open
    if (mappedInput.getHeldTime() > SKIP_PAGE_MS) {
      confirmingDelete = true;
      requestUpdate();
    } else {
      auto bookmark = bookmarks.at(selectorIndex);
      setResult(ProgressChangeResult{bookmark.spineIndex, bookmark.pageIndex});
      finish();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }

  buttonNavigator.onNextRelease([this] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, bookmarks.size());
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, bookmarks.size());
    requestUpdate();
  });
}

void EpubReaderBookmarksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  // Landscape orientation: reserve a horizontal gutter for button hints.
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Inverted portrait: reserve vertical space for hints at the top.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 50 : 20;
  // Landscape CW places hints on the left edge; CCW keeps them on the right.
  const int contentX = isLandscapeCw ? hintGutterWidth : 20;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;
  const int numBookmarks = bookmarks.size();
  const int pageItems = getPageItems();

  // Manual centering to honor content gutters.
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_BOOKMARKS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_BOOKMARKS), true, EpdFontFamily::BOLD);

  if (numBookmarks > 0) {
    if (confirmingDelete) {
      GUI.drawHelpText(renderer, Rect{0, pageHeight / 2 - LINE_HEIGHT * 2, pageWidth, LINE_HEIGHT}, tr(STR_CONFIRM_DELETE_BOOKMARK));

      // render list with just the selected item for the user to confirm to delete
      GUI.drawList(
          renderer, Rect{0, pageHeight / 2, pageWidth, LINE_HEIGHT * pageItems}, 1, 0,
            [this](int index) {
              return bookmarks.at(selectorIndex).summary;
            }, 
            [this](int index) {
              auto bookmark = bookmarks.at(selectorIndex);
              auto tocIndex = epub->getTocIndexForSpineIndex(bookmark.spineIndex);
              auto tocTitle = (tocIndex >= 0) ? (epub->getTocItem(tocIndex)).title : tr(STR_UNNAMED);
              return std::to_string(bookmark.bookPercent) + "% - " + std::to_string(bookmark.chapterProgress) + "/" + std::to_string(bookmark.chapterPageCount) +
                    " - " + tocTitle;
            }, [](int index) { return UIIcon::BookmarkFilled; });
    } else {
      GUI.drawList(
          renderer, Rect{0, LINE_HEIGHT + contentY, pageWidth, LINE_HEIGHT * pageItems}, numBookmarks, selectorIndex,
            [this](int index) {
              return bookmarks.at(index).summary;
            },
            [this](int index) {
              auto bookmark = bookmarks.at(index);
              auto tocIndex = epub->getTocIndexForSpineIndex(bookmark.spineIndex);
              auto tocTitle = (tocIndex >= 0) ? (epub->getTocItem(tocIndex)).title : tr(STR_UNNAMED);
              return std::to_string(bookmark.bookPercent) + "% - " + std::to_string(bookmark.chapterProgress) + "/" + std::to_string(bookmark.chapterPageCount) +
                    " - " + tocTitle;
            }, [](int index) { return UIIcon::BookmarkFilled; });
      
      GUI.drawHelpText(renderer, Rect{0, pageHeight - 80, pageWidth, LINE_HEIGHT}, tr(STR_HOLD_CONFIRM_TO_DELETE));
    }
  } else {
    GUI.drawHelpText(renderer, Rect{0, LINE_HEIGHT * 2, pageWidth, LINE_HEIGHT}, tr(STR_BOOKMARK_INSTRUCTIONS));
  }

  const auto backLabel = confirmingDelete ? tr(STR_CANCEL) : tr(STR_BACK);
  const auto confirmLabel = confirmingDelete ? tr(STR_DELETE) : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
