#include "EpubReaderClippingListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

#include "ClippingStore.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "clippings/ClippingPreview.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {

constexpr int ENTER_DELETE_MODE_MS = 700;

}  // namespace

EpubReaderClippingListActivity::EpubReaderClippingListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("EpubReaderClippings", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

void EpubReaderClippingListActivity::onEnter() {
  UiListActivity::onEnter();
  initialListRender = true;
  for (auto& preview : previews) {
    preview.reserve(clippingPreview::MAX_BYTES + clippingPreview::ELLIPSIS_BYTES);
  }
  rebuildRows();
}

int EpubReaderClippingListActivity::listCount() const { return static_cast<int>(CLIPPINGS.clippingCount()); }

const char* EpubReaderClippingListActivity::headerTitle() const { return tr(STR_CLIPPINGS); }

void EpubReaderClippingListActivity::rebuildRows() {
  windowStart = -1;
  windowCount = 0;
}

void EpubReaderClippingListActivity::refreshRowWindow(const int start) {
  const int total = listCount();
  int clamped = std::max(0, start);
  if (clamped > total - ROW_WINDOW) clamped = std::max(0, total - ROW_WINDOW);
  if (clamped == windowStart) return;

  windowCount = std::min(ROW_WINDOW, total - clamped);
  for (int slot = 0; slot < windowCount; ++slot) {
    const size_t clippingIndex = static_cast<size_t>(clamped + slot);
    const Clipping* clipping = CLIPPINGS.clippingAt(clippingIndex);
    previews[slot].clear();
    if (!clipping || !CLIPPINGS.readClippingPreview(clippingIndex, previews[slot])) {
      LOG_ERR("CLIP", "Failed to read clipping %u", static_cast<unsigned>(clippingIndex));
    }

    fui::ListItem item;
    item.label = clipping && clipping->chapterTitle[0] != '\0' ? clipping->chapterTitle : tr(STR_CLIPPINGS);
    item.subtitle = previews[slot].c_str();
    item.icon = listIconFor(UIIcon::Bookmark, 32);
    item.actionValue = static_cast<int16_t>(clippingIndex);
    rowItems[slot] = item;
  }
  windowStart = clamped;
}

void EpubReaderClippingListActivity::openSelected() {
  if (nav.selected < 0 || nav.selected >= listCount()) return;
  const Clipping* clipping = CLIPPINGS.clippingAt(static_cast<size_t>(nav.selected));
  std::string text;
  if (!clipping || !CLIPPINGS.readClippingText(static_cast<size_t>(nav.selected), text)) return;
  const std::string title = clipping->chapterTitle[0] != '\0' ? clipping->chapterTitle : tr(STR_CLIPPINGS);
  auto detail = makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, title, std::move(text));
  if (!detail) {
    LOG_ERR("CLIP", "Failed to allocate clipping detail activity");
    return;
  }
  startActivityForResult(std::move(detail), [this](const ActivityResult&) { requestUpdate(); });
}

void EpubReaderClippingListActivity::activateIndex(const int index) {
  if (confirmPopup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  openSelected();
}

void EpubReaderClippingListActivity::onRowLongPress(const int index) {
  if (confirmPopup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  showDeleteConfirmation();
}

void EpubReaderClippingListActivity::showDeleteConfirmation() {
  if (listCount() == 0 || confirmPopup.isActive()) return;
  confirmingDelete = true;
  const char* options[] = {tr(STR_CANCEL), tr(STR_DELETE)};
  confirmPopup.show(tr(STR_DELETE_CLIPPING_CONFIRM), options, 2, 0, [this](const int index) {
    confirmingDelete = false;
    if (index == 1) deleteSelected();
    requestUpdate();
  });
  requestUpdate();
}

void EpubReaderClippingListActivity::deleteSelected() {
  if (nav.selected < 0 || nav.selected >= listCount()) return;
  if (!CLIPPINGS.removeClippingAt(static_cast<size_t>(nav.selected))) {
    LOG_ERR("CLIP", "Failed to delete clipping %d", static_cast<int>(nav.selected));
    return;
  }
  rebuildRows();
  if (listCount() == 0) {
    finish();
    return;
  }
  if (nav.selected >= listCount()) nav.selected = listCount() - 1;
  nav.follow(listCount());
}

bool EpubReaderClippingListActivity::handleCustomInput() {
  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (confirmingDelete) {
    confirmingDelete = false;
    requestUpdate();
    return true;
  }
  return false;
}

bool EpubReaderClippingListActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() > ENTER_DELETE_MODE_MS) {
      showDeleteConfirmation();
    } else {
      openSelected();
    }
    return true;
  }
  return false;
}

void EpubReaderClippingListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (listCount() == 0) {
    screen.centeredText(tr(STR_NO_CLIPPINGS), screen.theme().bodyText);
    return;
  }

  if (!mappedInput.hasTouch()) {
    const int helpHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const fui::Rect band = screen.takeBottom(static_cast<int16_t>(helpHeight + metrics.verticalSpacing));
    GUI.drawHelpText(renderer, Rect{band.x, band.y + metrics.verticalSpacing, band.width, helpHeight},
                     tr(STR_HOLD_OPEN_TO_DELETE));
  }

  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  refreshRowWindow(nav.top);
  props.items = rowItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(windowCount);
  screen.list(props);
}

void EpubReaderClippingListActivity::render(RenderLock&& lock) {
  if (initialListRender && listCount() > 0) {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  }
  initialListRender = false;
  UiListActivity::render(std::move(lock));
  if (confirmPopup.processRender(renderer, mappedInput)) return;
}
