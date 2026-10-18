#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "SerialTestApi.h"
#include "components/UITheme.h"
#include "components/themes/ListTouchBand.h"
#include "fontIds.h"

int EpubReaderChapterSelectionActivity::getTotalItems() const { return epub->getTocItemsCount(); }

int EpubReaderChapterSelectionActivity::getPageItems() const {
  constexpr int lineHeight = 30;
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);
  const int startY = 60 + contentRect.y;
  const int availableHeight = contentRect.y + contentRect.height - startY - lineHeight;
  // Clamp to at least one item to avoid division by zero and empty paging.
  return std::max(1, availableHeight / lineHeight);
}

void EpubReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  selectorIndex = (currentTocIndex >= 0 && currentTocIndex < epub->getTocItemsCount())
                      ? currentTocIndex
                      : epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (selectorIndex == -1) {
    selectorIndex = 0;
  }

  // Trigger first update
  requestUpdate();
}

void EpubReaderChapterSelectionActivity::onExit() { Activity::onExit(); }

void EpubReaderChapterSelectionActivity::loop() {
  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Confirm && ev.type == ButtonEventManager::PressType::Short) {
      const auto newSpineIndex = epub->getSpineIndexForTocIndex(selectorIndex);
      if (newSpineIndex == -1) {
        ActivityResult result;
        result.isCancelled = true;
        setResult(std::move(result));
        finish();
      } else {
        setResult(ChapterResult{newSpineIndex, selectorIndex});
        finish();
      }
      return;
    }
    if (ev.button == MappedInputManager::Button::Back && ev.type == ButtonEventManager::PressType::Short) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
      return;
    }
  }

  // Up/Down step one chapter, Left/Right jump a screenful — a book with hundreds of chapters is
  // otherwise only crossable by holding a button down.
  buttonNavigator.onNextList(selectorIndex, totalItems, [this] { requestUpdate(); }, pageItems);
  buttonNavigator.onPreviousList(selectorIndex, totalItems, [this] { requestUpdate(); }, pageItems);
}

void EpubReaderChapterSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect contentRect = UITheme::getContentRect(renderer, true, true);
  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

  // Manual centering to honor content gutters.
  const int titleX =
      contentRect.x +
      (contentRect.width - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_SELECT_CHAPTER), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentRect.y, tr(STR_SELECT_CHAPTER), true, EpdFontFamily::BOLD);

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  // Highlight only the content area, not the hint gutters.
  renderer.fillRect(contentRect.x, 60 + contentRect.y + (selectorIndex % pageItems) * 30 - 2, contentRect.width - 1,
                    30);

  // Publish the rows for touch, from the same numbers the fill and the text use. The band top
  // is the selection fill's y (-2), not the text baseline: the fill is what reads as the row.
  ListTouchBand::recordUniformRows(contentRect.x, contentRect.width - 1, 60 + contentRect.y - 2, 30, pageStartIndex,
                                   std::min(pageItems, totalItems - pageStartIndex));

  for (int i = 0; i < pageItems; i++) {
    int itemIndex = pageStartIndex + i;
    if (itemIndex >= totalItems) break;
    const int displayY = 60 + contentRect.y + i * 30;
    const bool isSelected = (itemIndex == selectorIndex);

    auto item = epub->getTocItem(itemIndex);
    if (isSelected) SerialTestApi::noteSelection(itemIndex, item.title.c_str());  // CMD:STATE sel=, label=

    // Indent per TOC level while keeping content within the gutter-safe region.
    const int indentSize = contentRect.x + 20 + (item.level - 1) * 15;
    const std::string chapterName =
        renderer.truncatedText(UI_10_FONT_ID, item.title.c_str(), contentRect.width - 40 - indentSize);

    renderer.drawText(UI_10_FONT_ID, indentSize, displayY, chapterName.c_str(), !isSelected);
  }

  // Left/Right page when there is more than one page to cross, and fall back to stepping (which is
  // what ButtonNavigator::nextPageIndex does on a short list) when there is not.
  const bool pages = totalItems > pageItems;
  // Paging rides logical Left/Right and stepping logical Up/Down, so which pair sits on the front
  // strip and which on the side buttons is the orientation's business — mapHints routes both sets
  // of labels to whichever buttons are doing the job.
  const auto hints = mappedInput.mapHints(tr(STR_BACK), tr(STR_SELECT), pages ? tr(STR_LIST_PAGE_PREV) : "",
                                          pages ? tr(STR_LIST_PAGE_NEXT) : "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.front.btn1, hints.front.btn2, hints.front.btn3, hints.front.btn4);
  GUI.drawSideButtonHints(renderer, hints.side.up, hints.side.down);

  renderer.displayBuffer();
}

ListRowTap::Result EpubReaderChapterSelectionActivity::selectListRow(const int index) {
  return ListRowTap::apply(index, getTotalItems(), selectorIndex);
}
