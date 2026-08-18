#include "EpubReaderFootnotesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

EpubReaderFootnotesActivity::EpubReaderFootnotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                         const std::vector<FootnoteEntry>& footnotes)
    : UiListActivity("EpubReaderFootnotes", renderer, mappedInput), footnotes(footnotes) {
  buildRowItems();
}

// footnotes never changes after construction (no reload path), so this only
// needs to run once here rather than on every buildScreen() call.
void EpubReaderFootnotesActivity::buildRowItems() {
  rowItems.clear();
  rowItems.reserve(footnotes.size());
  for (const auto& footnote : footnotes) {
    fui::ListItem item;
    item.label = footnote.number[0] ? footnote.number : tr(STR_LINK);
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
}

void EpubReaderFootnotesActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  // The activated row leaves this screen; a lingering flash would gray an
  // unrelated element on the next render.
  app.clearTapFlash();
  setResult(FootnoteResult{footnotes[index].href});
  finish();
}

bool EpubReaderFootnotesActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    activateIndex(nav.selected);
    return true;
  }

  return false;
}

void EpubReaderFootnotesActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band GUI.drawHeader paints.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (footnotes.empty()) {
    screen.centeredText(tr(STR_NO_FOOTNOTES), screen.theme().bodyText);
    return;
  }

  // rowItems is built once in the constructor (see buildRowItems()) and
  // reused here on every repaint.
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}

void EpubReaderFootnotesActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Header via GUI.drawHeader (already FreeInkUI-themed); the rest of the
  // screen renders through the app.
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_FOOTNOTES));
}

void EpubReaderFootnotesActivity::drawFooter() {
  const auto labels = footnotes.empty() ? mappedInput.mapLabels(tr(STR_BACK), "", "", "")
                                        : mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
