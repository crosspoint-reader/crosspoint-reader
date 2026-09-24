#include "UiListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#if FREEINK_CAP_TOUCH
#include "components/UiAppHelpers.h"
#endif
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
#if FREEINK_CAP_TOUCH
  revealedIndex.store(-1);
  app.on(ACTION_SWIPE_DELETE, &UiListActivity::swipeDeleteTrampoline, this);
#endif
  app.setScreen(&UiListActivity::screenTrampoline, this);
  requestUpdate();
}

void UiListActivity::screenTrampoline(UiScreen& screen, void* user) {
  static_cast<UiListActivity*>(user)->buildScreen(screen);
}

void UiListActivity::rowActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<UiListActivity*>(user);
  if (event.value < 0 || event.value >= self->listCount()) return;
#if FREEINK_CAP_TOUCH
  if (self->revealedIndex.load() >= 0) {
    self->closeSwipeDelete();
    self->app.clearTapFlash();
    return;
  }
#endif
  self->onRowAction(event);
}

#if FREEINK_CAP_TOUCH
void UiListActivity::swipeDeleteTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<UiListActivity*>(user);
  if (event.value != self->revealedIndex.load() || !self->canSwipeDelete(event.value)) return;
  self->closeSwipeDelete();
  self->app.clearTapFlash();
  self->swipeDelete(event.value);
}

void UiListActivity::closeSwipeDelete() {
  if (revealedIndex.exchange(-1) >= 0) requestUpdate();
}

void UiListActivity::configureSwipeDelete(UiScreen& screen, fui::ListProps& props) {
  const int row = revealedIndex.load();
  if (row < 0 || row >= listCount() || !canSwipeDelete(row)) return;
  swipeReveal.index = static_cast<int16_t>(row);
  swipeReveal.action = ACTION_SWIPE_DELETE;
  swipeReveal.icon = fui::bitmapFromIcon(icon_trash_2_24);
  swipeReveal.width = static_cast<int16_t>(screen.theme().rowHeight + screen.theme().spaceSm * 2);
  props.reveal = &swipeReveal;
}

bool UiListActivity::handleSwipeDeleteInput() {
  if (!mappedInput.hasTouch()) return false;
  const int openRow = revealedIndex.load();
  if (openRow >= 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      closeSwipeDelete();
      return true;
    }
    int x = 0;
    int y = 0;
    if (mappedInput.wasScreenTapped(x, y)) {
      fui::Interaction hit;
      bool onDelete;
      {
        RenderLock lock(*this);
        onDelete = routingReady() && app.hitPublished(x, y, ACTION_SWIPE_DELETE, hit) && hit.value == openRow;
        if (!onDelete && routingReady())
          app.route(fui::InputSnapshot{.touchReleased = true, .touchX = -1, .touchY = -1});
      }
      if (!onDelete) {
        closeSwipeDelete();
        return true;
      }
    }
  }

  int x = 0;
  int y = 0;
  const auto swipe = mappedInput.wasSwipe(&x, &y);
  if (swipe == MappedInputManager::SwipeDir::Right && openRow >= 0) {
    UiAppHost::routeTouch(mappedInput, wantsTouchLongPress);
    closeSwipeDelete();
    return true;
  }
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    closeSwipeDelete();
    return false;
  }
  if (swipe != MappedInputManager::SwipeDir::Left) return false;

  fui::Interaction hit;
  {
    RenderLock lock(*this);
    if (!routingReady() || !app.hitPublished(x, y, ACTION_ROW, hit) || !canSwipeDelete(hit.value)) return false;
  }
  UiAppHost::routeTouch(mappedInput, wantsTouchLongPress);
  revealedIndex.store(hit.value);
  requestUpdate();
  return true;
}
#endif

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
#if FREEINK_CAP_TOUCH
  closeSwipeDelete();
#endif
  activeNav().requestSelection(index);
  requestUpdate();
}

void UiListActivity::loop() {
  if (handleCustomInput()) return;
#if FREEINK_CAP_TOUCH
  if (handleSwipeDeleteInput()) return;
#endif
  if (handleButtons()) return;
  if (routeListTouch()) return;

  // Swipes scroll the viewport; the selection stays put (it may scroll
  // off-screen) and button navigation pulls the view back to it.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    auto& n = activeNav();
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? n.inputPageRows() : -n.inputPageRows();
    LOG_DBG("LIST", "%s swipe delta=%d count=%d", name.c_str(), delta, listCount());
    n.requestScroll(delta);
    requestUpdate();
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
  // overshoots and rows between pages would never be shown. The measurement
  // can be one build old while a refresh is in flight; the next layout's
  // feedback corrects the viewport.
  buttonNavigator.onNextContinuous(
      [this, count, &n] { moveSelectionTo(ButtonNavigator::nextPageIndex(n.selected, count, n.inputPageRows())); });
  buttonNavigator.onPreviousContinuous(
      [this, count, &n] { moveSelectionTo(ButtonNavigator::previousPageIndex(n.selected, count, n.inputPageRows())); });
}

void UiListActivity::syncListViewport(UiScreen& screen, fui::ListProps& props, const int selectionOffset) {
  props.partialTrailingRow = true;
  auto& n = activeNav();
  const int prevTop = n.top;
  const bool trusted = n.trusts(listCount());
  const int drawn = n.drawnRows;

  screen.syncListViewport(n, props, listCount(), selectionOffset);

  // When the selection is already visible in the current viewport (based on
  // the measured drawnRows rather than the unweighted visibleRows estimate),
  // keep selection-follow anchored instead of jumping to top. Explicit swipe
  // scrolling clears followPending and must retain its new viewport.
  if (n.followPending && trusted && drawn > 0) {
    const int sel = props.selectedIndex;
    if (sel >= prevTop && sel < prevTop + drawn) {
      n.top = prevTop;
      props.topIndex = static_cast<uint16_t>(prevTop);
    }
  }
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
}
