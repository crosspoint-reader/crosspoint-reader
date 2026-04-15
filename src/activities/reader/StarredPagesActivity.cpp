#include "StarredPagesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

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

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (totalItems == 0) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto& bm = bookmarkStore.getAll()[selectorIndex];
    setResult(StarredPageResult{bm.spineIndex, bm.pageNumber});
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

  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

  buttonNavigator.onNextRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });
}

void StarredPagesActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_STARRED_PAGES));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;

  const int totalItems = static_cast<int>(bookmarkStore.getAll().size());

  if (totalItems == 0) {
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, contentTop + 20,
                      tr(STR_NO_STARRED_PAGES));
  } else {
    GUI.drawList(renderer, Rect{contentRect.x, contentTop, contentRect.width, contentHeight}, totalItems, selectorIndex,
                 [this](int index) { return getItemLabel(index); });
  }

  const bool hasItems = totalItems > 0;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), hasItems ? tr(STR_SELECT) : "",
                                            hasItems ? tr(STR_RENAME) : "", hasItems ? tr(STR_DELETE) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  renderer.displayBuffer();
}
