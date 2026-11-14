#pragma once
#include <FreeInkUICore.h>

// While a contact is a tap candidate, touchSnapshotFrom() repeats the press at
// the landing point. The press hit test needs that point, but a drag must
// follow the live position. One snapshot holds one position, so route the
// press first, then the hold at liveX/liveY. A bare press dispatches nothing.
// Split only when the router already has a bound element (touchBound).
// Otherwise this may be the first routed frame, and the hold would bind at the
// live position. Routed whole, the frame binds and reports at the landing
// point; later held frames follow the live position.
template <typename RouteFn>
freeink::ui::ActionEvent routePressThenHold(const freeink::ui::InputSnapshot& snap, const int16_t liveX,
                                            const int16_t liveY, const bool touchBound, RouteFn&& route) {
  if (!touchBound || !snap.touchPressed || !snap.touchHeld || snap.touchReleased) return route(snap);

  freeink::ui::InputSnapshot press{};
  press.touchPressed = true;
  press.touchX = snap.touchX;
  press.touchY = snap.touchY;
  route(press);

  freeink::ui::InputSnapshot hold{};
  hold.touchHeld = true;
  hold.touchX = liveX;
  hold.touchY = liveY;
  return route(hold);
}
