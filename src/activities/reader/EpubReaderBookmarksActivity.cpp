#include "EpubReaderBookmarksActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "../util/ConfirmationActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

namespace {
constexpr unsigned long DELETE_HOLD_MS = 1000;

bool wasReleasedMoveListForward(const MappedInputManager& mappedInput) {
  // Footer DIR_UP/DIR_DOWN mapLabels use front Left/Right (see MappedInputManager::mapLabels).
  // Logical Up/Down are the fixed side buttons — accept both like other reader sublists.
  return mappedInput.wasReleased(MappedInputManager::Button::Right) ||
         mappedInput.wasReleased(MappedInputManager::Button::Down);
}
bool wasReleasedMoveListBack(const MappedInputManager& mappedInput) {
  return mappedInput.wasReleased(MappedInputManager::Button::Left) ||
         mappedInput.wasReleased(MappedInputManager::Button::Up);
}
}  // namespace

EpubReaderBookmarksActivity::EpubReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                         const std::shared_ptr<Epub>& epub, const int currentSpineIndex,
                                                         const int currentPageNumber)
    : Activity("EpubReaderBookmarks", renderer, mappedInput),
      epub(epub),
      currentSpineIndex(currentSpineIndex),
      currentPageNumber(currentPageNumber) {}

int EpubReaderBookmarksActivity::getTotalItems() const { return static_cast<int>(bookmarks.size()); }

int EpubReaderBookmarksActivity::getListStartY() const {
  const auto orientation = renderer.getOrientation();
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;
  const int titleTop = 15 + contentY;
  const int subtitleY = titleTop + renderer.getLineHeight(UI_12_FONT_ID) + 4;
  return subtitleY + renderer.getLineHeight(UI_10_FONT_ID) + 10;
}

int EpubReaderBookmarksActivity::getPageItems() const {
  constexpr int lineHeight = 30;
  const int screenHeight = renderer.getScreenHeight();
  const int startY = getListStartY();
  const int availableHeight = screenHeight - startY - lineHeight;
  return std::max(1, availableHeight / lineHeight);
}

void EpubReaderBookmarksActivity::onEnter() {
  Activity::onEnter();

  bookmarks.clear();
  bookmarks.reserve(EpubBookmarksStore::MAX_BOOKMARKS);
  if (epub) {
    if (!EpubBookmarksStore::load(*epub, bookmarks)) {
      bookmarks.clear();
    }
  }

  // Default selection: first matching current position, otherwise first item.
  selectorIndex = 0;
  for (int i = 0; i < static_cast<int>(bookmarks.size()); i++) {
    if (bookmarks[i].spineIndex == static_cast<uint16_t>(currentSpineIndex) &&
        bookmarks[i].pageNumber == static_cast<uint16_t>(currentPageNumber)) {
      selectorIndex = i;
      break;
    }
  }

  requestUpdate();
}

void EpubReaderBookmarksActivity::onExit() { Activity::onExit(); }

std::string EpubReaderBookmarksActivity::getBookmarkFullLabel(const EpubBookmark& b) const {
  std::string chapterName = tr(STR_UNNAMED);
  if (epub) {
    const int tocIndex = epub->getTocIndexForSpineIndex(static_cast<int>(b.spineIndex));
    if (tocIndex >= 0) {
      chapterName = epub->getTocItem(tocIndex).title;
      if (chapterName.empty()) {
        chapterName = tr(STR_UNNAMED);
      }
    }
  }

  std::string label = chapterName;
  label += " \xe2\x80\x94 ";  // UTF-8 em dash (—)
  label += std::string(tr(STR_PAGE_SHORT)) + std::to_string(static_cast<int>(b.pageNumber) + 1);
  return label;
}

std::string EpubReaderBookmarksActivity::getBookmarkLabel(const EpubBookmark& b, const int maxWidth) const {
  const std::string full = getBookmarkFullLabel(b);
  return renderer.truncatedText(UI_10_FONT_ID, full.c_str(), maxWidth);
}

void EpubReaderBookmarksActivity::promptDeleteSelected() {
  if (!epub || bookmarks.empty() || selectorIndex < 0 || selectorIndex >= static_cast<int>(bookmarks.size())) {
    return;
  }
  const auto b = bookmarks[selectorIndex];

  std::string heading = tr(STR_DELETE) + std::string("? ");
  const std::string body = getBookmarkFullLabel(b);

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, body, true /* wrapBody */, 10),
      [this, b](const ActivityResult& result) {
        if (result.isCancelled || !epub) {
          return;
        }
        if (!EpubBookmarksStore::remove(*epub, b.spineIndex, b.pageNumber)) {
          return;
        }
        awaitingBookmarkRemovedAck = true;
        EpubBookmarksStore::load(*epub, bookmarks);
        if (selectorIndex >= static_cast<int>(bookmarks.size())) {
          selectorIndex = std::max(0, static_cast<int>(bookmarks.size()) - 1);
        }
        requestUpdate();
      });
}

void EpubReaderBookmarksActivity::loop() {
  const int totalItems = getTotalItems();

  if (awaitingBookmarkRemovedAck) {
    // Dismiss banner. List keys (front Left/Right per hints, optional side Up/Down)
    if (wasReleasedMoveListForward(mappedInput) && totalItems > 0) {
      awaitingBookmarkRemovedAck = false;
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
      requestUpdate();
      return;
    }
    if (wasReleasedMoveListBack(mappedInput) && totalItems > 0) {
      awaitingBookmarkRemovedAck = false;
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      awaitingBookmarkRemovedAck = false;
      requestUpdate();
      return;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  // Direct list keys (no ButtonNavigator)
  if (totalItems > 0) {
    if (wasReleasedMoveListForward(mappedInput)) {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
      requestUpdate();
      return;
    }
    if (wasReleasedMoveListBack(mappedInput)) {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() >= DELETE_HOLD_MS) {
      promptDeleteSelected();
      return;
    }

    if (totalItems <= 0 || selectorIndex < 0 || selectorIndex >= totalItems) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
      return;
    }

    const auto b = bookmarks[selectorIndex];
    setResult(BookmarkResult{static_cast<int>(b.spineIndex), static_cast<int>(b.pageNumber)});
    finish();
    return;
  }
}

void EpubReaderBookmarksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

  const int titleTop = 15 + contentY;
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_BOOKMARKS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, titleTop, tr(STR_BOOKMARKS), true, EpdFontFamily::BOLD);

  const int subtitleY = titleTop + renderer.getLineHeight(UI_12_FONT_ID) + 4;
  const char* sub = tr(STR_BOOKMARKS_LONG_HOLD_HINT);
  const int subW = renderer.getTextWidth(UI_10_FONT_ID, sub);
  const int subtitleX = contentX + (contentWidth - subW) / 2;
  renderer.drawText(UI_10_FONT_ID, subtitleX, subtitleY, sub, true);

  const int listStartY = getListStartY();

  if (totalItems <= 0) {
    const int emptyX = contentX + (contentWidth - renderer.getTextWidth(UI_10_FONT_ID, tr(STR_NO_BOOKMARKS))) / 2;
    renderer.drawText(UI_10_FONT_ID, emptyX, listStartY + 40, tr(STR_NO_BOOKMARKS));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (awaitingBookmarkRemovedAck) {
      GUI.drawPopup(renderer, tr(STR_BOOKMARK_REMOVED));
    } else {
      renderer.displayBuffer();
    }
    return;
  }

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  renderer.fillRect(contentX, listStartY + (selectorIndex % pageItems) * 30 - 2, contentWidth - 1, 30);

  for (int i = 0; i < pageItems; i++) {
    const int itemIndex = pageStartIndex + i;
    if (itemIndex >= totalItems) break;
    const int displayY = listStartY + i * 30;
    const bool isSelected = (itemIndex == selectorIndex);
    const auto label = getBookmarkLabel(bookmarks[itemIndex], contentWidth - 40);
    renderer.drawText(UI_10_FONT_ID, contentX + 20, displayY, label.c_str(), !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (awaitingBookmarkRemovedAck) {
    GUI.drawPopup(renderer, tr(STR_BOOKMARK_REMOVED));
  } else {
    renderer.displayBuffer();
  }
}
