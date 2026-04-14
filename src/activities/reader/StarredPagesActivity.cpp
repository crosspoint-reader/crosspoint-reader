#include "StarredPagesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

int StarredPagesActivity::getPageItems() const {
  constexpr int lineHeight = 30;
  const int screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = 60 + hintGutterHeight;
  const int availableHeight = screenHeight - startY - lineHeight;
  return std::max(1, availableHeight / lineHeight);
}

std::string StarredPagesActivity::getDefaultLabel(int index) const {
  const auto& bm = bookmarkStore.getAll()[index];
  char buf[64];
  if (epub) {
    const int tocIndex = epub->getTocIndexForSpineIndex(bm.spineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      return tocItem.title + " - " + tr(STR_PAGE_PREFIX) + std::to_string(bm.pageNumber + 1);
    }
    snprintf(buf, sizeof(buf), "%s%d, %s%d", tr(STR_SECTION_PREFIX), bm.spineIndex + 1, tr(STR_PAGE_PREFIX),
             bm.pageNumber + 1);
  } else {
    snprintf(buf, sizeof(buf), "%s%d", tr(STR_PAGE_PREFIX), bm.pageNumber + 1);
  }
  return std::string(buf);
}

std::string StarredPagesActivity::getItemLabel(int index) const {
  char prefix[16];
  snprintf(prefix, sizeof(prefix), "%d. ", index + 1);
  const auto& bm = bookmarkStore.getAll()[index];
  return std::string(prefix) + (bm.name.empty() ? getDefaultLabel(index) : bm.name);
}

void StarredPagesActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void StarredPagesActivity::onExit() {
  bookmarkStore.save();
  Activity::onExit();
}

void StarredPagesActivity::startRename() {
  const auto& all = bookmarkStore.getAll();
  if (all.empty() || selectorIndex >= static_cast<int>(all.size())) return;
  const int renamingIndex = selectorIndex;
  const std::string initial =
      all[renamingIndex].name.empty() ? getDefaultLabel(renamingIndex) : all[renamingIndex].name;
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_RENAME), initial,
                                                                 BookmarkStore::MAX_NAME_LENGTH, false),
                         [this, renamingIndex](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             const auto& kr = std::get<KeyboardResult>(result.data);
                             bookmarkStore.rename(renamingIndex, kr.text);
                           }
                           requestUpdate();
                         });
}

void StarredPagesActivity::deleteSelected() {
  const auto& all = bookmarkStore.getAll();
  if (all.empty() || selectorIndex >= static_cast<int>(all.size())) return;
  bookmarkStore.removeAt(selectorIndex);
  const int remaining = static_cast<int>(bookmarkStore.getAll().size());
  if (remaining == 0) {
    // Nothing left — drop back to the reader.
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  if (selectorIndex >= remaining) selectorIndex = remaining - 1;
  requestUpdate();
}

void StarredPagesActivity::loop() {
  const int totalItems = static_cast<int>(bookmarkStore.getAll().size());
  const int pageItems = getPageItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (totalItems > 0) {
      const auto& bm = bookmarkStore.getAll()[selectorIndex];
      setResult(StarredPageResult{bm.spineIndex, bm.pageNumber});
      finish();
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

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    startRename();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    deleteSelected();
    return;
  }

  // Side buttons (Up/Down) drive list navigation; Left/Right are reserved for rename/delete.
  buttonNavigator.onRelease({MappedInputManager::Button::Down}, [this, totalItems] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onRelease({MappedInputManager::Button::Up}, [this, totalItems] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onContinuous({MappedInputManager::Button::Down}, [this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });

  buttonNavigator.onContinuous({MappedInputManager::Button::Up}, [this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });
}

void StarredPagesActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int totalItems = static_cast<int>(bookmarkStore.getAll().size());

  if (totalItems == 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_NO_STARRED_PAGES), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

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

  // Title
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_STARRED_PAGES), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_STARRED_PAGES), true, EpdFontFamily::BOLD);

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;

  // Highlight selection
  renderer.fillRect(contentX, 60 + contentY + (selectorIndex % pageItems) * 30 - 2, contentWidth - 1, 30);

  for (int i = 0; i < pageItems; i++) {
    int itemIndex = pageStartIndex + i;
    if (itemIndex >= totalItems) break;
    const int displayY = 60 + contentY + i * 30;
    const bool isSelected = (itemIndex == selectorIndex);

    const std::string label = renderer.truncatedText(UI_10_FONT_ID, getItemLabel(itemIndex).c_str(), contentWidth - 40);
    renderer.drawText(UI_10_FONT_ID, contentX + 20, displayY, label.c_str(), !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_RENAME), tr(STR_DELETE));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
