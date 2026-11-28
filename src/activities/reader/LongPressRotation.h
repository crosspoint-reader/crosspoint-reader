#pragma once

#include <cstdint>

// Long-press orientation change: next page turns right (index - 1), previous left.
// A page button swapped by orientation (isNavDirectionSwapped) keeps its direction.
namespace longpressrotation {

// Front Left/Right always follow the swap; side buttons only in the Prev/Next and Next/Prev layouts.
constexpr bool heldButtonSwapped(const bool navSwapped, const bool frontHeld, const bool sideHeld,
                                 const bool sideLayoutFollowsOrientation) {
  return navSwapped && (frontHeld || (sideHeld && sideLayoutFollowsOrientation));
}

constexpr bool turnsRight(const bool nextTriggered, const bool heldButtonSwapped) {
  return nextTriggered != heldButtonSwapped;
}

constexpr uint8_t nextOrientation(const uint8_t current, const bool turnRight, const uint8_t count) {
  return turnRight ? static_cast<uint8_t>((current + count - 1) % count) : static_cast<uint8_t>((current + 1) % count);
}

}  // namespace longpressrotation
