#pragma once

#include <atomic>
#include <cstdint>

#include "TouchUi.h"

// Where the four bottom button hints actually landed, so a tap can be matched against the
// strip that is really on screen.
//
// Split from ButtonHintLayout.h, which decides where the boxes GO and needs BoardConfig
// (and therefore Arduino.h) to do it. This half only records where they WENT, and its
// consumers are the input side, which has no business depending on board geometry. Keeping
// it free-standing also makes the hit test host-testable -- the arithmetic is the part most
// likely to be wrong and the part hardest to check on a device.
//
// Coordinates are PORTRAIT logical px: both drawButtonHints() implementations force Portrait
// for the duration of the draw and restore afterwards, so that is the frame these numbers are
// in, whatever the screen is rotated to. A hit test must therefore resolve its tap into the
// Portrait frame as well -- GfxRenderer::tapToLogical(Orientation, ...) exists for exactly
// that -- rather than against the live orientation. Doing so makes the strip tappable in all
// four orientations; matching against live coords would only work while the screen is
// portrait, and would silently hit the wrong box otherwise.
//
// Index i is raw hardware button i. mapLabels() emits the four labels in
// {BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT} order and permutes only the text, so a hit
// needs no further mapping and honours the user's button remapping for free.
namespace ButtonHintStrip {

struct Strip {
  int y = 0;
  int height = 0;
  int x[4] = {0, 0, 0, 0};
  // Per box: a box grows when its label needs the room (HintLayout).
  int width[4] = {0, 0, 0, 0};
  // False for an empty label. BaseTheme skips such a box entirely and LyraTheme shrinks it
  // to a decorative stub, so in neither case is there something there for a finger to mean.
  bool active[4] = {false, false, false, false};
};

// Same idea for the two side hints (BTN_UP, BTN_DOWN), drawSideButtonHints() rather than
// drawButtonHints(). Kept as a separate small strip rather than folded into Strip above: the
// side boxes are independent rects (X3 sits them side by side at one y, X4 stacks them in one
// column), so there is no shared y/height/width band to factor out the way the bottom strip
// has.
//
// Slot 0 is BTN_UP, slot 1 is BTN_DOWN -- the fixed physical order MappedInputManager::mapHints()
// hands to drawSideButtonHints() regardless of orientation, and the side buttons are not
// remappable -- so, like Strip, a hit needs no further mapping beyond that fixed slot order.
struct SideStrip {
  int x[2] = {0, 0};
  int y[2] = {0, 0};
  int width[2] = {0, 0};
  int height[2] = {0, 0};
  bool active[2] = {false, false};
};

// Published across tasks: drawButtonHints() runs on the render task, the hit test on the
// loop task. A seqlock rather than a bare valid flag, because a half-written Strip would be
// hit-tested against a mix of two screens' geometry. Writes are frequent and reads rare, so
// losing a read to a concurrent write costs nothing -- a failed snapshot reads as "no strip
// this tick" and the tap falls through to whoever else wants it, which is the safe way to
// be wrong.
#if CP_TOUCH_UI
namespace detail {
inline Strip& storage() {
  static Strip s;
  return s;
}
inline std::atomic<uint32_t>& seq() {
  static std::atomic<uint32_t> s{0};
  return s;
}
}  // namespace detail

// Called by drawButtonHints() with the geometry it just painted.
inline void record(const Strip& in) {
  auto& seq = detail::seq();
  seq.fetch_add(1, std::memory_order_acq_rel);  // odd: write in progress
  detail::storage() = in;
  seq.fetch_add(1, std::memory_order_release);  // even: stable
}

// Forget the strip. Must be called on every activity transition: a screen that draws no
// hints would otherwise inherit the previous screen's boxes and turn taps near the bottom
// edge into phantom button presses.
inline void invalidate() { record(Strip{}); }

// Reads the strip if one is stable. False on a torn read or when none was ever recorded.
inline bool snapshot(Strip& out) {
  auto& seq = detail::seq();
  const uint32_t before = seq.load(std::memory_order_acquire);
  if (before == 0 || (before & 1u) != 0) return false;
  out = detail::storage();
  return seq.load(std::memory_order_acquire) == before;
}

// True when the current screen painted at least one tappable box. Lets a caller skip the
// tap queue entirely on a screen with no strip, leaving the tap for whatever else wants it.
inline bool hasStrip() {
  Strip s;
  if (!snapshot(s)) return false;
  return s.height > 0 && (s.active[0] || s.active[1] || s.active[2] || s.active[3]);
}

// Pure geometry, split out so the arithmetic can be tested without the shared state.
// Returns 0..3 for the box containing the point, or -1 for a miss.
inline int hitTestIn(const Strip& s, const int px, const int py) {
  if (s.height <= 0) return -1;
  if (py < s.y || py >= s.y + s.height) return -1;
  for (int i = 0; i < 4; ++i) {
    if (!s.active[i]) continue;
    if (px >= s.x[i] && px < s.x[i] + s.width[i]) return i;
  }
  return -1;
}

// Returns 0..3 (= raw hardware button index) for the hint box containing the point, or -1
// for a miss, a torn read, or a strip that was never recorded.
inline int hitTest(const int px, const int py) {
  Strip s;
  if (!snapshot(s)) return -1;
  return hitTestIn(s, px, py);
}

// Same seqlock scheme as above, published/consumed independently of the bottom strip so a
// screen carrying both (the common case) does not have one draw's write torn by the other's.
namespace Side {
namespace detail {
inline SideStrip& storage() {
  static SideStrip s;
  return s;
}
inline std::atomic<uint32_t>& seq() {
  static std::atomic<uint32_t> s{0};
  return s;
}
}  // namespace detail

// Called by drawSideButtonHints() with the geometry it just painted.
inline void record(const SideStrip& in) {
  auto& seq = detail::seq();
  seq.fetch_add(1, std::memory_order_acq_rel);
  detail::storage() = in;
  seq.fetch_add(1, std::memory_order_release);
}

inline void invalidate() { record(SideStrip{}); }

inline bool snapshot(SideStrip& out) {
  auto& seq = detail::seq();
  const uint32_t before = seq.load(std::memory_order_acquire);
  if (before == 0 || (before & 1u) != 0) return false;
  out = detail::storage();
  return seq.load(std::memory_order_acquire) == before;
}

inline bool hasStrip() {
  SideStrip s;
  if (!snapshot(s)) return false;
  return s.active[0] || s.active[1];
}

// Returns 0 (BTN_UP) or 1 (BTN_DOWN) for the box containing the point, or -1 for a miss.
inline int hitTestIn(const SideStrip& s, const int px, const int py) {
  for (int i = 0; i < 2; ++i) {
    if (!s.active[i]) continue;
    if (px < s.x[i] || px >= s.x[i] + s.width[i]) continue;
    if (py < s.y[i] || py >= s.y[i] + s.height[i]) continue;
    return i;
  }
  return -1;
}

inline int hitTest(const int px, const int py) {
  SideStrip s;
  if (!snapshot(s)) return -1;
  return hitTestIn(s, px, py);
}
}  // namespace Side

#else  // !CP_TOUCH_UI

// Strip itself stays defined above: the themes still PAINT the hint strip on a
// board with no digitiser, and construct one to do it. What goes is the published
// copy -- which nothing there hit-tests -- and the seqlock that guarded it.
inline void record(const Strip&) {}
inline void invalidate() {}
inline bool snapshot(Strip&) { return false; }
inline bool hasStrip() { return false; }
inline int hitTestIn(const Strip&, int, int) { return -1; }
inline int hitTest(int, int) { return -1; }

namespace Side {
inline void record(const SideStrip&) {}
inline void invalidate() {}
inline bool snapshot(SideStrip&) { return false; }
inline bool hasStrip() { return false; }
inline int hitTestIn(const SideStrip&, int, int) { return -1; }
inline int hitTest(int, int) { return -1; }
}  // namespace Side

#endif  // CP_TOUCH_UI

}  // namespace ButtonHintStrip
