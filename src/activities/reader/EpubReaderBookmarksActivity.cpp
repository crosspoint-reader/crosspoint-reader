#include "EpubReaderBookmarksActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

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

  selectorIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (selectorIndex == -1) {
    selectorIndex = 0;
  }

  LOG_DBG("EPB", "Load %d bookmarks", getPageItems());
  bookmarkUtil.load(getPageItems());

  // Trigger first update
  requestUpdate();
}

void EpubReaderBookmarksActivity::onExit() { Activity::onExit(); }

void EpubReaderBookmarksActivity::loop() {
  const int pageItems = getPageItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) { // Open
    if (bookmarkUtil.doesBookmarkExist(selectorIndex)) {
      auto bookmark = *bookmarkUtil.getBookmark(selectorIndex);
      setResult(ProgressChangeResult{bookmark.currentSpineIndex, bookmark.currentPage});
      finish();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) { // Delete
    bookmarkUtil.deleteBookmark(selectorIndex);
    requestUpdate();
    return; // return to not process navigator input
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) { // Set
    bookmarkUtil.saveBookmark(selectorIndex, currentSpineIndex, currentPage, pageCount);
    requestUpdate();
    return; // return to not process navigator input
  }

  buttonNavigator.onNextRelease([this, pageItems] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, pageItems] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, pageItems);
    requestUpdate();
  });
}

void EpubReaderBookmarksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
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
  const int numBookmarks = getPageItems();
  
  // Manual centering to honor content gutters.
  const int titleX =
  contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_BOOKMARKS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_BOOKMARKS), true, EpdFontFamily::BOLD);  

  GUI.drawList(
      renderer, Rect{0, LINE_HEIGHT + contentY, pageWidth, LINE_HEIGHT * numBookmarks}, numBookmarks, selectorIndex,
      [this](int index) {
        if (bookmarkUtil.doesBookmarkExist(index)) {
          auto bookmark = bookmarkUtil.getBookmark(index).value();
          auto item = epub->getTocItem(epub->getTocIndexForSpineIndex(bookmark.currentSpineIndex));
          return item.title;
        } else {
          return std::string();
        }
      },
      [this](int index) { 
        if (bookmarkUtil.doesBookmarkExist(index)) {
          auto bookmark = bookmarkUtil.getBookmark(index).value();
          const float chapterProgress = static_cast<float>(bookmark.currentPage) / static_cast<float>(bookmark.pageCount);
          float bookProgress = epub->calculateProgress(bookmark.currentSpineIndex, chapterProgress) * 100.0f;
          return std::to_string(bookmark.currentPage + 1) + "/" + std::to_string(bookmark.pageCount) + " - " + std::to_string(static_cast<int>(bookProgress)) + "%";
        } else {
          return std::string();
        }
      }, [](int index) { return UIIcon::Bookmark; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DELETE), bookmarkUtil.doesBookmarkExist(selectorIndex) ? tr(STR_OVERWRITE) : tr(STR_NEW));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}


