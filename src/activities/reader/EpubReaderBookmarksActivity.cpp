#include "EpubReaderBookmarksActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

int EpubReaderBookmarksActivity::getPageItems() const {
  // Layout constants used in renderScreen
  constexpr int lineHeight = 60;

  const int screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  // In inverted portrait, the button hints are drawn near the logical top.
  // Reserve vertical space so list items do not collide with the hints.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = 60 + hintGutterHeight;
  const int availableHeight = screenHeight - startY - lineHeight;
  // Clamp to at least one item to avoid division by zero and empty paging.
  return std::max(1, availableHeight / lineHeight);
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
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) { // Set
    bookmarkUtil.saveBookmark(selectorIndex, currentSpineIndex, currentPage);
    requestUpdate();
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
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  // Landscape CW places hints on the left edge; CCW keeps them on the right.
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;
  
  // Manual centering to honor content gutters.
  const int titleX =
  contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_BOOKMARKS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_BOOKMARKS), true, EpdFontFamily::BOLD);
  
  const int numBookmarks = getPageItems();
  // Highlight only the content area, not the hint gutters.
  renderer.fillRect(contentX, 60 + contentY + (selectorIndex % numBookmarks) * 30 - 2, contentWidth - 1, 30);

  for (int i = 0; i < numBookmarks; i++) {
    const int displayY = 60 + contentY + i * 30;
    const bool isSelected = (i == selectorIndex);
    if (bookmarkUtil.doesBookmarkExist(i)) {
      auto bookmark = bookmarkUtil.getBookmark(i);
      auto item = epub->getTocItem(epub->getTocIndexForSpineIndex(bookmark->currentSpineIndex));

      // Indent per TOC level while keeping content within the gutter-safe region.
      const int indentSize = contentX + 20 + (item.level - 1) * 15;
      const std::string chapterName =
          renderer.truncatedText(UI_10_FONT_ID, item.title.c_str(), contentWidth - 40 - indentSize);

      renderer.drawText(UI_10_FONT_ID, indentSize, displayY, chapterName.c_str(), !isSelected);
    } else {
      renderer.drawText(UI_10_FONT_ID, contentX, displayY, tr(STR_EMPTY_SLOT), !isSelected, EpdFontFamily::ITALIC);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DELETE), bookmarkUtil.doesBookmarkExist(selectorIndex) ? tr(STR_OVERWRITE) : tr(STR_SAVE));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}


