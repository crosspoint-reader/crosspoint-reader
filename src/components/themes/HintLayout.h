#pragma once

#include <algorithm>
#include <cstdint>

// How big the button hint boxes are and where they sit once a label needs more than the tuned box.
//
// Both kinds of hint box used to be a fixed size: 80/106 px along the bottom strip, 78/80 px up
// the side. That is room for "Up" and "Right", not for "Вверх / Вкладка" or "Вправо / Вкладка":
// the side labels ran out of both ends of their boxes and over each other, the bottom ones were
// clipped to "Вправо ..." with room to spare on the strip (issue #446).
//
// So a box now grows to fit its label, by one rule for both kinds -- as far as the label needs,
// never smaller than the tuned box, never past the room it has (grow()) -- and only a label that
// still does not fit is shrunk and then clipped (BaseTheme::fitHintLabel()). Labels that fit the
// tuned boxes leave every box exactly where it was measured to sit.
//
// Everything here is in the PANEL coordinates (the Portrait draw frame) the themes draw the
// hints in, whatever the orientation, and is pure arithmetic so it is host-testable.
namespace HintLayout {

// As long as `needed`, never shorter than the theme's tuned `base`, never longer than `room`.
// When even `base` does not fit the room, base wins: that is the size the box always had.
constexpr int grow(const int base, const int needed, const int room) { return std::max(base, std::min(needed, room)); }

// ---- Bottom strip ------------------------------------------------------------------------------
//
// A grown box may take an equal share of the panel (all four at that width still fit). It stays
// centred on its tuned position where it can, and the boxes are pushed apart only as far as they
// must be to keep clear of each other and of the panel edges: the least-squares placement under
// those constraints. A long label moves its neighbours a few pixels at most.

inline constexpr int BOTTOM_COUNT = 4;
// Text margin inside a bottom box, both ends together.
inline constexpr int BOTTOM_LABEL_PADDING = 4;
// Closest a grown box may come to its neighbour and to the panel edge. A tuned layout that
// already sits closer than this keeps its own spacing.
inline constexpr int BOTTOM_MIN_GAP = 4;
inline constexpr int BOTTOM_EDGE_MARGIN = 4;

namespace detail {
inline int gapAfter(const int base, const int (&tuned)[BOTTOM_COUNT], const int i) {
  return std::min(BOTTOM_MIN_GAP, tuned[i + 1] - tuned[i] - base);
}
inline int leftMargin(const int (&tuned)[BOTTOM_COUNT]) { return std::min(BOTTOM_EDGE_MARGIN, tuned[0]); }
inline int rightMargin(const int screenWidth, const int base, const int (&tuned)[BOTTOM_COUNT]) {
  return std::min(BOTTOM_EDGE_MARGIN, screenWidth - tuned[BOTTOM_COUNT - 1] - base);
}
}  // namespace detail

// The widest a bottom box may grow: an equal share of what is left of the panel once the gaps
// and margins are taken out, and never more than a quarter of it. Never less than `base`.
inline int bottomMaxWidth(const int screenWidth, const int base, const int (&tuned)[BOTTOM_COUNT]) {
  int room = screenWidth - detail::leftMargin(tuned) - detail::rightMargin(screenWidth, base, tuned);
  for (int i = 0; i + 1 < BOTTOM_COUNT; ++i) room -= detail::gapAfter(base, tuned, i);
  return std::max(base, std::min(room / BOTTOM_COUNT, screenWidth / BOTTOM_COUNT));
}

// Left edge of each bottom box, given each box's width (each within [base, bottomMaxWidth()]).
inline void placeBottom(const int screenWidth, const int base, const int (&tuned)[BOTTOM_COUNT],
                        const int (&widths)[BOTTOM_COUNT], int (&out)[BOTTOM_COUNT]) {
  // Shift each box's wanted left edge by the room the boxes before it need. The constraint
  // "box i+1 starts after box i plus the gap" then becomes "the shifted values never decrease",
  // which pool-adjacent-violators solves exactly: pool any run that decreases into its mean.
  int offset[BOTTOM_COUNT];
  int target[BOTTOM_COUNT];
  for (int i = 0; i < BOTTOM_COUNT; ++i) {
    offset[i] = i == 0 ? 0 : offset[i - 1] + widths[i - 1] + detail::gapAfter(base, tuned, i - 1);
    target[i] = tuned[i] + (base - widths[i]) / 2 - offset[i];  // centred on the tuned box
  }

  int sum[BOTTOM_COUNT];
  int count[BOTTOM_COUNT];
  int blocks = 0;
  for (int i = 0; i < BOTTOM_COUNT; ++i) {
    sum[blocks] = target[i];
    count[blocks] = 1;
    ++blocks;
    while (blocks > 1 && sum[blocks - 2] * count[blocks - 1] > sum[blocks - 1] * count[blocks - 2]) {
      sum[blocks - 2] += sum[blocks - 1];
      count[blocks - 2] += count[blocks - 1];
      --blocks;
    }
  }

  // Keep the whole run on the panel. Clamping a non-decreasing sequence keeps it
  // non-decreasing, so the gaps survive.
  const int lo = detail::leftMargin(tuned);
  const int hi =
      screenWidth - detail::rightMargin(screenWidth, base, tuned) - widths[BOTTOM_COUNT - 1] - offset[BOTTOM_COUNT - 1];
  if (hi < lo) {  // widths beyond bottomMaxWidth(): nothing sensible to do but the tuned layout
    std::copy(tuned, tuned + BOTTOM_COUNT, out);
    return;
  }
  for (int b = 0, i = 0; b < blocks; ++b) {
    const int value = std::clamp(sum[b] / count[b], lo, hi);
    for (int k = 0; k < count[b]; ++k, ++i) out[i] = value + offset[i];
  }
}

// ---- Side (Up/Down) boxes ----------------------------------------------------------------------
//
// The column they stand in is reserved along the panel's whole edge -- getContentRect() takes
// sideButtonHintsWidth off it from end to end -- so a box can simply be longer, within the
// stretch of the column nothing else is drawn in.

// Text margin inside a side box, both ends together.
inline constexpr int SIDE_LABEL_PADDING = 8;

// The stretch of the column the boxes may occupy, as [top, bottom).
struct Span {
  int top;
  int bottom;
};

// Which panel end the screen header (the logical top) lies across, if it crosses the side
// column at all: Portrait puts it at the panel top, PortraitInverted at the panel bottom, and
// in landscape the logical top runs along a different panel edge entirely.
enum class HeaderEdge : uint8_t { None, PanelTop, PanelBottom };

// The bottom hint strip always sits on the panel's bottom edge (drawButtonHints() draws in
// panel coordinates too), so that end is always taken. In PortraitInverted the header lies
// beyond it, since getContentRect() puts the strip at the logical top there.
constexpr Span sideFreeSpan(const int screenHeight, const int bottomStripHeight, const int headerBand,
                            const HeaderEdge header) {
  Span span{0, screenHeight - bottomStripHeight};
  if (header == HeaderEdge::PanelTop) span.top = headerBand;
  if (header == HeaderEdge::PanelBottom) span.bottom -= headerBand;
  return span;
}

struct Box {
  int y;
  int height;
};

struct Stack {
  Box up;
  Box down;
};

// X4: both boxes in one column, Up above Down. `junction` is the bottom edge of Up's tuned box
// and Down starts `gap` below it. Up grows upwards and Down downwards, so the pair stays centred
// on the two buttons; both take the same length, the longer label's, so they read as a pair.
constexpr Stack sideStacked(const int junction, const int gap, const int base, const int needed, const Span span) {
  const int room = std::min(junction - span.top, span.bottom - (junction + gap));
  const int length = grow(base, needed, room);
  return {{junction - length, length}, {junction + gap, length}};
}

// X3: one box on each side edge, level with its button. Grows equally both ways from `centre`.
constexpr Box sideCentred(const int centre, const int base, const int needed, const Span span) {
  const int room = 2 * std::min(centre - span.top, span.bottom - centre);
  const int length = grow(base, needed, room);
  return {centre - length / 2, length};
}

}  // namespace HintLayout
