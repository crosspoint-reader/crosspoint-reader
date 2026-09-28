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
  result.event = app.route(result.snap);
  return result;
}

fui::ActionEvent UiAppHost::route(const fui::InputSnapshot& snap) {
  if (!uiReady) return {};
  return app.route(snap);
}
