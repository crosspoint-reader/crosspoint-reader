#include "XtcReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "ButtonEventManager.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

int XtcReaderChapterSelectionActivity::getPageItems() const {
  constexpr int lineHeight = 30;
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);
  constexpr int startY = 60;
  const int availableHeight = contentRect.height - startY - lineHeight;
  // Clamp to at least one item to prevent empty page math.
  return std::max(1, availableHeight / lineHeight);
}

int XtcReaderChapterSelectionActivity::findChapterIndexForPage(uint32_t page) const {
  if (!xtc) {
    return 0;
  }

  const auto& chapters = xtc->getChapters();
  for (size_t i = 0; i < chapters.size(); i++) {
    if (page >= chapters[i].startPage && page <= chapters[i].endPage) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

void XtcReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();

  if (!xtc) {
    return;
  }

  selectorIndex = findChapterIndexForPage(currentPage);

  requestUpdate();
}

void XtcReaderChapterSelectionActivity::onExit() { Activity::onExit(); }

void XtcReaderChapterSelectionActivity::loop() {
  if (!xtc) {
    return;
  }

  const int pageItems = getPageItems();
  const int totalItems = static_cast<int>(xtc->getChapters().size());

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Confirm && ev.type == ButtonEventManager::PressType::Short) {
      const auto& chapters = xtc->getChapters();
      if (!chapters.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(chapters.size())) {
        setResult(PageResult{chapters[selectorIndex].startPage});
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

  buttonNavigator.onNextRelease([this, totalItems] {
    if (ButtonEventManager::hasDoubleAction(MappedInputManager::Button::Right) ||
        ButtonEventManager::hasDoubleAction(MappedInputManager::Button::PageForward)) {
      return;
    }
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, totalItems] {
    if (ButtonEventManager::hasDoubleAction(MappedInputManager::Button::Left) ||
        ButtonEventManager::hasDoubleAction(MappedInputManager::Button::PageBack)) {
      return;
    }
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

void XtcReaderChapterSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect contentRect = UITheme::getContentRect(renderer, true, false);
  const int pageItems = getPageItems();

  const int titleX =
      contentRect.x +
      (contentRect.width - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_SELECT_CHAPTER), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, contentRect.y + 15, tr(STR_SELECT_CHAPTER), true, EpdFontFamily::BOLD);

  const auto& chapters = xtc->getChapters();
  if (chapters.empty()) {
    const int emptyX =
        contentRect.x + (contentRect.width - renderer.getTextWidth(UI_10_FONT_ID, tr(STR_NO_CHAPTERS))) / 2;
    renderer.drawText(UI_10_FONT_ID, emptyX, contentRect.y + 120, tr(STR_NO_CHAPTERS));
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  renderer.fillRect(contentRect.x, contentRect.y + 60 + (selectorIndex % pageItems) * 30 - 2, contentRect.width - 1,
                    30);
  for (int i = pageStartIndex; i < static_cast<int>(chapters.size()) && i < pageStartIndex + pageItems; i++) {
    const auto& chapter = chapters[i];
    const char* title = chapter.name.empty() ? tr(STR_UNNAMED) : chapter.name.c_str();
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, contentRect.y + 60 + (i % pageItems) * 30, title,
                      i != selectorIndex);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
