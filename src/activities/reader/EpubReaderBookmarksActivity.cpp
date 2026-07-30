#include "EpubReaderBookmarksActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "../../util/BookmarkFile.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int ENTER_DELETE_MODE_MS = 700;

// Layout constants used in renderScreen
constexpr int LINE_HEIGHT = 60;
}  // namespace

void EpubReaderBookmarksActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  if (!BookmarkFile::load(epubPath, bookmarks)) {
    bookmarks.shrink_to_fit();
  }
  LOG_DBG("EPB", "Loaded %d bookmarks for book: %s", static_cast<int>(bookmarks.size()), epubPath.c_str());

  // Trigger first update
  requestUpdate();
}

void EpubReaderBookmarksActivity::onExit() { Activity::onExit(); }

int EpubReaderBookmarksActivity::getGutterBottom(const GfxRenderer& renderer) {
  const auto orientation = renderer.getOrientation();
  const bool isPortrait = orientation == GfxRenderer::Orientation::Portrait;
  return isPortrait ? 75 : 40;  // Reserve vertical space for button hints at the bottom
}

int EpubReaderBookmarksActivity::getListHeight(const GfxRenderer& renderer) {
  const auto pageHeight = renderer.getScreenHeight();
  return pageHeight - getGutterBottom(renderer) - LINE_HEIGHT;  // Reserve vertical space for title and button hints
}

void EpubReaderBookmarksActivity::loop() {
  auto openBookmark = [this] {
    if (bookmarks.empty()) {
      return;
    }
    auto bookmark = bookmarks.at(selectorIndex);
    ProgressChangeResult result{};
    result.xpath = bookmark.xpath;
    result.percentage = bookmark.percentage;
    result.hasSavedProgress = true;
    if (bookmark.computedChapterPageCount > 0 && bookmark.computedChapterProgress < bookmark.computedChapterPageCount &&
        bookmark.computedSpineIndex < epub->getSpineItemsCount()) {
      result.spineIndex = bookmark.computedSpineIndex;
      result.page = bookmark.computedChapterProgress;
      result.totalPages = bookmark.computedChapterPageCount;
    }
    setResult(std::move(result));
    finish();
  };

  // Delete confirmation popup
  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (confirmingDelete) {
    // Popup dismissed without a selection (Back button or tap outside): cancel delete
    confirmingDelete = false;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  const auto orientation = renderer.getOrientation();
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 40 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;
  const int contentY = isPortraitInverted ? 50 : 0;
  const int listY = contentY + LINE_HEIGHT;
  const int listHeight = getListHeight(renderer);
  int tapped = 0;
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTouchDown(tx, ty) && tx >= contentX && tx < contentX + contentWidth &&
      mappedInput.wasListItemTouchedDown(tapped, static_cast<int>(bookmarks.size()), selectorIndex, listY, listHeight,
                                         true)) {
    if (selectorIndex != tapped) {
      selectorIndex = tapped;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasScreenTapped(tx, ty) && tx >= contentX && tx < contentX + contentWidth &&
      mappedInput.wasListItemTapped(tapped, static_cast<int>(bookmarks.size()), selectorIndex, listY, listHeight,
                                    true)) {
    selectorIndex = tapped;
    openBookmark();
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up && !bookmarks.empty()) {
    selectorIndex =
        ButtonNavigator::nextPageIndex(selectorIndex, bookmarks.size(), GUI.getListPageItems(listHeight, true));
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down && !bookmarks.empty()) {
    selectorIndex =
        ButtonNavigator::previousPageIndex(selectorIndex, bookmarks.size(), GUI.getListPageItems(listHeight, true));
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {  // Open
    openBookmark();
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() > ENTER_DELETE_MODE_MS) {
    if (bookmarks.empty()) {
      return;
    }
    confirmingDelete = true;
    const char* options[] = {tr(STR_CANCEL), tr(STR_DELETE)};
    confirmPopup.show(tr(STR_CONFIRM_DELETE_BOOKMARK), options, 2, 0, [this](int idx) {
      confirmingDelete = false;
      if (idx == 1) {
        deleteSelectedBookmark();
      }
      requestUpdate();
    });
    requestUpdate();
  }

  buttonNavigator.onNextRelease([this] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, bookmarks.size());
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, bookmarks.size());
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this] {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, bookmarks.size(),
                                                   GUI.getListPageItems(getListHeight(renderer), true));
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, bookmarks.size(),
                                                       GUI.getListPageItems(getListHeight(renderer), true));
    requestUpdate();
  });
}

void EpubReaderBookmarksActivity::deleteSelectedBookmark() {
  bookmarks.erase(bookmarks.begin() + selectorIndex);
  if (!BookmarkFile::save(epubPath, bookmarks)) {
    LOG_ERR("EPB", "Failed to save bookmarks after delete");
  }

  // Move selector up if we deleted the last item
  if (selectorIndex >= bookmarks.size() && selectorIndex > 0) {
    selectorIndex--;
  }

  if (bookmarks.empty()) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }
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
  const bool isPortrait = orientation == GfxRenderer::Orientation::Portrait;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 40 : 0;
  // Landscape CW places hints on the left edge; CCW keeps them on the right.
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int hintGutterBottom = getGutterBottom(renderer);
  const int contentY = hintGutterHeight;
  const int listY = contentY + LINE_HEIGHT;  // Reserve vertical space for title
  const int listHeight = getListHeight(renderer);
  const int numBookmarks = bookmarks.size();

  // Manual centering to honor content gutters.
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_BOOKMARKS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_BOOKMARKS), true, EpdFontFamily::BOLD);

  const auto getBookmarkTitle = [this](int index) {
    return bookmarks.at(confirmingDelete ? selectorIndex : index).summary;
  };
  const auto getBookmarkSubtitle = [this](int index) {
    auto bookmark = bookmarks.at(confirmingDelete ? selectorIndex : index);
    auto tocIndex = epub->getTocIndexForSpineIndex(bookmark.computedSpineIndex);
    auto tocTitle = (tocIndex >= 0) ? (epub->getTocItem(tocIndex)).title : tr(STR_UNNAMED);
    std::string subtitle = std::to_string((int)(std::clamp(bookmark.percentage, 0.0f, 1.0f) * 100.0f + 0.5f)) + "% - ";
    if (bookmark.computedChapterPageCount > 0) {
      subtitle += std::to_string(bookmark.computedChapterProgress + 1) + "/" +
                  std::to_string(bookmark.computedChapterPageCount) + " - ";
    }
    return subtitle + tocTitle;
  };
  const auto getBookmarkIcon = [isPortrait](int index) {
    // only enabled icon in portrait mode due to limitation with rotating icons for other orientations
    return isPortrait ? UIIcon::Bookmark : UIIcon::None;
  };

  if (numBookmarks > 0) {
    if (confirmingDelete) {
      // Render just the selected item near the top; the confirmation popup occupies the center
      GUI.drawList(renderer, Rect{contentX, listY, contentWidth, LINE_HEIGHT}, 1, 0, getBookmarkTitle,
                   getBookmarkSubtitle, getBookmarkIcon);
    } else {
      GUI.drawList(renderer, Rect{contentX, listY, contentWidth, listHeight}, numBookmarks, selectorIndex,
                   getBookmarkTitle, getBookmarkSubtitle, getBookmarkIcon);

      GUI.drawHelpText(renderer, Rect{contentX, pageHeight - hintGutterBottom, contentWidth, LINE_HEIGHT},
                       tr(STR_HOLD_OPEN_TO_DELETE));
    }
  }

  if (confirmPopup.processRender(renderer, mappedInput)) return;

  const auto confirmLabel = bookmarks.size() > 0 ? tr(STR_SELECT) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
