#include "UiAppHost.h"

#include "UiAppHelpers.h"

namespace fui = freeink::ui;

UiAppHost::UiAppHost(const GfxRenderer& renderer)
    : uiTarget(makeUiTarget(renderer)), app(uiTarget, uiTarget.deviceContext()) {}

void UiAppHost::resetUi() {
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
}

void UiAppHost::renderUi() {
  app.setDevice(uiTarget.deviceContext());
  app.render();
  uiReady = true;
}

UiAppHost::TouchRoute UiAppHost::routeTouch(const MappedInputManager& input, const bool withLongPress,
                                            const bool routeHeld) {
  TouchRoute result;  // named apart from route() — cppcheck flags the shadow
  if (!uiReady) return result;
  result.snap = touchSnapshotFrom(input, withLongPress);
  if (!result.snap.touchPressed && !result.snap.touchReleased && !(routeHeld && result.snap.touchHeld)) {
    return result;
  }
  result.routed = true;
  result.event = route(result.snap);
  return result;
}

fui::ActionEvent UiAppHost::route(const fui::InputSnapshot& snap) {
  if (!uiReady) return {};
  const auto event = app.route(snap);
  // Releases activate controls; held/pressed frames only highlight or drag.
  // Empty/disabled hit regions produce no event and therefore no feedback.
  if (event && (snap.touchReleased || snap.swipeLeft || snap.swipeRight)) {
    haptic_feedback::touchAction(event.longPress);
  }
  return event;
}

void UiAppHost::syncListViewport(UiScreen& screen, fui::ListNav& n, fui::ListProps& props, const int count,
                                 const int selectionOffset) {
  props.toggleCheckbox = true;
  props.toggleWidth = 28;
  props.toggleHeight = 28;
  props.partialTrailingRow = true;
  const int prevTop = n.top;
  const bool trusted = n.trusts(count);
  const int drawn = n.drawnRows;

  screen.syncListViewport(n, props, count, selectionOffset);

  // Keep visible selections anchored. At the next row, prevent only a
  // backwards tail clamp; ordinary forward following must work in one build.
  // Swipes retain their independent viewport.
  if (n.followPending && trusted && drawn > 0) {
    const int sel = props.selectedIndex;
    const int pageEnd = prevTop + drawn;
    if (sel >= prevTop && (sel < pageEnd || (sel == pageEnd && n.top < prevTop))) {
      n.top = prevTop;
      props.topIndex = static_cast<uint16_t>(prevTop);
    }
  }
}
