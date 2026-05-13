#include "MdReaderTocSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "components/UITheme.h"
#include "fontIds.h"

int MdReaderTocSelectionActivity::getTotalItems() const { return static_cast<int>(headings.size()); }

int MdReaderTocSelectionActivity::getPageItems() const {
  constexpr int lineHeight = 30;
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);
  const int startY = 60 + contentRect.y;
  const int availableHeight = contentRect.y + contentRect.height - startY - lineHeight;
  return std::max(1, availableHeight / lineHeight);
}

void MdReaderTocSelectionActivity::onEnter() {
  Activity::onEnter();

  if (selectorIndex < 0 || selectorIndex >= getTotalItems()) {
    selectorIndex = 0;
  }

  requestUpdate();
}

void MdReaderTocSelectionActivity::onExit() { Activity::onExit(); }

void MdReaderTocSelectionActivity::loop() {
  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Confirm && ev.type == ButtonEventManager::PressType::Short) {
      if (selectorIndex >= 0 && selectorIndex < totalItems) {
        setResult(PageResult{static_cast<uint32_t>(headings[selectorIndex].pageIndex)});
      } else {
        ActivityResult result;
        result.isCancelled = true;
        setResult(std::move(result));
      }
      finish();
      return;
    }

    if (ev.button == MappedInputManager::Button::Back && ev.type == ButtonEventManager::PressType::Short) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
      return;
    }

    if ((ev.button == MappedInputManager::Button::PageBack || ev.button == MappedInputManager::Button::Left) &&
        ev.type == ButtonEventManager::PressType::Short) {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
      requestUpdate();
      return;
    }

    if ((ev.button == MappedInputManager::Button::PageForward || ev.button == MappedInputManager::Button::Right) &&
        ev.type == ButtonEventManager::PressType::Short) {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
      requestUpdate();
      return;
    }
  }

  buttonNavigator.onNextList(selectorIndex, totalItems, [this] { requestUpdate(); });
  buttonNavigator.onPreviousList(selectorIndex, totalItems, [this] { requestUpdate(); });
}

void MdReaderTocSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect contentRect = UITheme::getContentRect(renderer, true, false);
  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

  const int titleX =
      contentRect.x +
      (contentRect.width - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_SELECT_CHAPTER), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentRect.y, tr(STR_SELECT_CHAPTER), true, EpdFontFamily::BOLD);

  const int pageStartIndex = selectorIndex / pageItems * pageItems;
  renderer.fillRect(contentRect.x, 60 + contentRect.y + (selectorIndex % pageItems) * 30 - 2, contentRect.width - 1,
                    30);

  for (int i = 0; i < pageItems; i++) {
    int itemIndex = pageStartIndex + i;
    if (itemIndex >= totalItems) break;
    const int displayY = 60 + contentRect.y + i * 30;
    const bool isSelected = (itemIndex == selectorIndex);

    const auto& heading = headings[itemIndex];
    const int indentRelative = 20 + (heading.level - 1) * 10;
    const int drawX = contentRect.x + indentRelative;
    const std::string title =
        renderer.truncatedText(UI_10_FONT_ID, heading.title.c_str(), contentRect.width - 40 - indentRelative);
    renderer.drawText(UI_10_FONT_ID, drawX, displayY, title.c_str(), !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
