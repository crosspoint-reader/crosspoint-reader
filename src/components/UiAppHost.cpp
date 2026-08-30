#include "UiAppHost.h"

#include "UiAppHelpers.h"

namespace fui = freeink::ui;

UiAppHost::UiAppHost(const GfxRenderer& renderer)
    : uiTarget(makeUiTarget(renderer)), app(uiTarget, uiDeviceContext(renderer, uiTarget)), uiRenderer(renderer) {}

void UiAppHost::resetUi() {
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
}

void UiAppHost::renderUi() {
  app.setDevice(uiDeviceContext(uiRenderer, uiTarget));
  app.render();
  uiReady = true;
}

fui::Insets UiAppHost::contentMargins(const GfxRenderer& renderer, const Rect& bounds) {
  const Rect content = UITheme::getContentArea(renderer);
  return fui::Insets{static_cast<int16_t>(bounds.y - content.y),
                     static_cast<int16_t>(content.x + content.width - bounds.x - bounds.width),
                     static_cast<int16_t>(content.y + content.height - bounds.y - bounds.height),
                     static_cast<int16_t>(bounds.x - content.x)};
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
