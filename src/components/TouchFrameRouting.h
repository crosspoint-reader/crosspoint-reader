#pragma once
#include <FreeInkUICore.h>

// While a contact is a tap candidate, touchSnapshotFrom() repeats the press at
// the landing point. The press hit test needs that point, but a drag must
// follow the live position. One snapshot holds one position, so route the
// press first, then the hold at liveX/liveY. A bare press dispatches nothing.
// Callers must route every held frame. The press flag appears only after a
// short delay, so an earlier held frame has already bound the drag at the
// landing point. Otherwise the hold below binds it at the live position.
template <typename RouteFn>
freeink::ui::ActionEvent routePressThenHold(const freeink::ui::InputSnapshot& snap, const int16_t liveX,
                                            const int16_t liveY, RouteFn&& route) {
  if (!snap.touchPressed || !snap.touchHeld || snap.touchReleased) return route(snap);

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
