#include "XtcReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <vector>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

XtcReaderChapterSelectionActivity::XtcReaderChapterSelectionActivity(GfxRenderer& renderer,
                                                                     MappedInputManager& mappedInput,
                                                                     const std::shared_ptr<Xtc>& xtc,
                                                                     const uint32_t currentPage)
    : UiListActivity("XtcReaderChapterSelection", renderer, mappedInput), xtc(xtc), currentPage(currentPage) {}

int XtcReaderChapterSelectionActivity::findChapterIndexForPage(const uint32_t page) const {
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
  UiListActivity::onEnter();

  if (!xtc) {
    return;
  }

  buildRowItems();

  // Open on the current chapter, which may sit past the first page; the first
  // screen build pulls the viewport to it (ListNav follow-on-build).
  nav.selected = findChapterIndexForPage(currentPage);
}

// Derives rowItems from the xtc's chapters. Called once from onEnter() since
// chapters are static for this screen's lifetime.
void XtcReaderChapterSelectionActivity::buildRowItems() {
  const auto& chapters = xtc->getChapters();
  rowItems.clear();
  rowItems.reserve(chapters.size());
  for (const auto& chapter : chapters) {
    fui::ListItem item;
    item.label = chapter.name.empty() ? tr(STR_UNNAMED) : chapter.name.c_str();
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
}

void XtcReaderChapterSelectionActivity::activateIndex(const int index) {
  // The activated row leaves this screen (finish); a lingering flash would
  // gray an unrelated element on the next render.
  app.clearTapFlash();
  const auto& chapters = xtc->getChapters();
  if (!chapters.empty() && index >= 0 && index < static_cast<int>(chapters.size())) {
    nav.selected = index;
    setResult(PageResult{chapters[index].startPage});
    finish();
  }
}

bool XtcReaderChapterSelectionActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }

  if (!xtc) {
    return true;  // no book: nothing else to route this pass
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(nav.selected);
    return true;
  }

  return false;
}

void XtcReaderChapterSelectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band drawChrome paints the title in.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (!xtc) {
    return;
  }
  if (rowItems.empty()) {
    screen.centeredText(tr(STR_NO_CHAPTERS), screen.theme().bodyText);
    return;
  }

  // rowItems is built once in onEnter() (see buildRowItems()) and reused
  // here on every repaint.
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}

void XtcReaderChapterSelectionActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  // Centered title in the header band the content margin reserves.
  const int titleWidth = renderer.getTextWidth(UI_12_FONT_ID, tr(STR_SELECT_CHAPTER), EpdFontFamily::BOLD);
  const int titleX = safe.x + (safe.width - titleWidth) / 2;
  const int titleY = safe.y + metrics.topPadding + (metrics.headerHeight - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, titleY, tr(STR_SELECT_CHAPTER), true, EpdFontFamily::BOLD);
}
