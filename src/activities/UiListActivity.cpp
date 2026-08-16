#include "UiListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

UiListActivity::UiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const bool wantsTouchLongPress)
    : Activity(name, renderer, mappedInput), UiAppHost(renderer), wantsTouchLongPress(wantsTouchLongPress) {}

void UiListActivity::onEnter() {
  Activity::onEnter();
  activeNav().reset();
  resetUi();
  app.on(ACTION_ROW, &UiListActivity::rowActionTrampoline, this);
  app.setScreen(&UiListActivity::screenTrampoline, this);
  requestUpdate();
}

void UiListActivity::screenTrampoline(UiScreen& screen, void* user) {
  static_cast<UiListActivity*>(user)->buildScreen(screen);
}

void UiListActivity::rowActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<UiListActivity*>(user);
  if (event.value < 0 || event.value >= self->listCount()) return;
  self->onRowAction(event);
}

void UiListActivity::onRowAction(const fui::ActionEvent& event) {
  activeNav().selected = event.value;
  if (event.longPress) {
    onRowLongPress(event.value);
    return;
  }
  activateIndex(event.value);
}

bool UiListActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBackButton();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int selected = activeNav().selected;
    if (selected >= 0 && selected < listCount()) activateIndex(selected);
    return true;
  }
  return false;
}

bool UiListActivity::routeListTouch() {
  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let the action trampoline dispatch.
  const auto route = UiAppHost::routeTouch(mappedInput, wantsTouchLongPress);
  // No pressed-state repaint: the render it triggers would drop a slow tap's
  // release inside the uiReady window (tap-to-activate needed two taps), and
  // it costs a second e-ink refresh per tap.
  if (route.routed && app.invalidated()) requestUpdate();
  return static_cast<bool>(route);  // dispatched to the action handler
}

void UiListActivity::moveSelectionTo(const int index) {
  auto& n = activeNav();
  const int previous = n.selected;
  n.selected = index;
  n.follow(listCount());
  // Arm the selection-delta fast path; render() falls back to a full repaint
  // unless the viewport provably didn't move. Any other requestUpdate source
  // (data reload, touch action, custom input) never arms it. Coalesced moves
  // (several presses before one render) keep the ORIGINAL from-row: that is
  // the row still highlighted in the framebuffer.
  if (previous != index) {
    if (!selectionDeltaPending) selectionDeltaFrom = previous;
    selectionDeltaPending = true;
  }
  requestUpdate();
}

void UiListActivity::loop() {
  if (handleCustomInput()) return;
  if (handleButtons()) return;
  if (routeListTouch()) return;

  // Swipes scroll the viewport; the selection stays put (it may scroll
  // off-screen) and button navigation pulls the view back to it.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    auto& n = activeNav();
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? n.pageRows() : -n.pageRows();
    if (n.scrollBy(delta, listCount())) requestUpdate();
    return;
  }

  navigateButtons();
}

void UiListActivity::navigateButtons() {
  const int count = listCount();
  auto& n = activeNav();
  buttonNavigator.onNextRelease([this, count, &n] { moveSelectionTo(ButtonNavigator::nextIndex(n.selected, count)); });
  buttonNavigator.onPreviousRelease(
      [this, count, &n] { moveSelectionTo(ButtonNavigator::previousIndex(n.selected, count)); });
  // Page by the rows the last build actually drew (pageRows), not the
  // fixed-height visibleRows estimate: with wrapped labels the estimate
  // overshoots and rows between pages would never be shown.
  buttonNavigator.onNextContinuous(
      [this, count, &n] { moveSelectionTo(ButtonNavigator::nextPageIndex(n.selected, count, n.pageRows())); });
  buttonNavigator.onPreviousContinuous(
      [this, count, &n] { moveSelectionTo(ButtonNavigator::previousPageIndex(n.selected, count, n.pageRows())); });
}

void UiListActivity::syncListViewport(UiScreen& screen, fui::ListProps& props, const bool hasSubtitle) {
  int16_t rowHeight = screen.theme().rowHeight;
  if (!mappedInput.hasTouch()) {
    // Non-touch hardware (X3/X4) keeps the original, denser per-theme row
    // height instead of FreeInkUI's touch-target-sized default, so lists fit
    // as many rows per screen as they did before the FreeInkUI migration.
    // props.rowHeight must be set explicitly: screen.list() otherwise falls
    // back to the (touch-friendly) theme token, not this local value.
    // A label that must wrap (labelText.maxLines > 1) grows only its own row:
    // list() sizes wrapped items per-row, so the dense height stays.
    const auto& metrics = UITheme::getInstance().getMetrics();
    rowHeight = static_cast<int16_t>(hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight);
    props.rowHeight = rowHeight;
  }
  activeNav().syncToProps(screen.body(), rowHeight, screen.theme().listRowGap, listCount(), props);
}

void UiListActivity::drawChrome() {
  const char* title = headerTitle();
  if (!title) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);
}

void UiListActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void UiListActivity::render(RenderLock&&) {
  const bool deltaPending = selectionDeltaPending;
  const int deltaFrom = selectionDeltaFrom;
  selectionDeltaPending = false;
  selectionDeltaFrom = -1;
  if (deltaPending && tryRenderSelectionDelta(deltaFrom)) {
    return;
  }

  renderer.clearScreen();
  drawChrome();
  renderUi();
  // Wrapped labels grow rows, so fewer rows can fit than the fixed-height
  // estimate ListNav plans with. list() reports the real layout back
  // (ListNav::onListRendered); when the selection landed past the drawn rows
  // the nav advanced the viewport and asked for another build. Bounded: top
  // strictly advances toward the selection each pass.
  for (int pass = 0; activeNav().consumeRebuildNeeded() && pass < 8; ++pass) {
    renderer.clearScreen();
    drawChrome();
    renderUi();
  }
  drawFooter();
  renderer.displayBuffer();
  lastRenderedTop = activeNav().top;
  partialRefreshCount = 0;
}

bool UiListActivity::tryRenderSelectionDelta(const int fromIndex) {
  auto& n = activeNav();
  // Preconditions: a completed full render to diff against (lastRenderedTop
  // is only set at the end of the full path), an unchanged viewport, both
  // rows tracked by the last build's layout, chrome that doesn't depend on
  // the selection, and the ghosting budget.
  if (fromIndex < 0 || fromIndex == n.selected) return false;
  if (selectionDeltaNeedsFullChrome()) return false;
  if (partialRefreshCount >= PARTIAL_REFRESH_LIMIT) return false;
  if (lastRenderedTop < 0 || n.top != lastRenderedTop) return false;
  fui::Rect fromRect{}, toRect{};
  if (!n.rowRectFor(fromIndex, &fromRect) || !n.rowRectFor(n.selected, &toRect)) return false;

  // Union of the two rows (rows share x/width; selection markers and row
  // insets live within the row rects).
  const int x0 = std::min(fromRect.x, toRect.x);
  const int y0 = std::min(fromRect.y, toRect.y);
  const int x1 = std::max(fromRect.right(), toRect.right());
  const int y1 = std::max(fromRect.bottom(), toRect.bottom());

  // Rebuild the frame under a draw clip: only ops touching the band repaint
  // (everything else is identical to the last frame by precondition). Clear
  // the band first — a deselected row's background may be transparent, which
  // would leave the old highlight underneath.
  renderer.setDrawClip(y0, y1 - y0);
  renderer.fillRect(x0, y0, x1 - x0, y1 - y0, false);
  renderUi();
  renderer.clearDrawClip();

  // Layout surprises (list mutated under us, selection clipped) fall back to
  // the full path with a coherent full repaint.
  if (n.consumeRebuildNeeded() || n.top != lastRenderedTop) return false;

  if (!renderer.displayRegion(x0, y0, x1 - x0, y1 - y0)) return false;
  partialRefreshCount++;
  return true;
}
