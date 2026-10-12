#pragma once

#include <algorithm>
#include <cstdint>

// How long the side (Up/Down) hint boxes are and where they sit along their edge.
//
// The boxes used to be a fixed 78-80 px: room for "Up", not for "Вверх / Вкладка", which ran out
// of both ends of its box and across the one beside it (issue #446). The column they stand in is
// reserved along the panel's whole edge -- getContentRect() takes sideButtonHintsWidth off it
// from end to end -- so a box can simply be longer. It grows only as far as the longer label
// needs, so the common short labels keep the hand-tuned boxes, and only within the stretch of
// the column nothing else is drawn in. A label that will not fit even then is shrunk and clipped
// the way the bottom hints are.
//
// Everything here is in the PANEL coordinates (the Portrait draw frame) the themes draw the side
// hints in, whatever the orientation.
namespace SideHintLayout {

// Along-edge padding around a label, both ends together.
inline constexpr int LABEL_PADDING = 8;

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
constexpr Span freeSpan(const int screenHeight, const int bottomStripHeight, const int headerBand,
                        const HeaderEdge header) {
  Span span{0, screenHeight - bottomStripHeight};
  if (header == HeaderEdge::PanelTop) span.top = headerBand;
  if (header == HeaderEdge::PanelBottom) span.bottom -= headerBand;
  return span;
}

// As long as `needed`, never shorter than the theme's tuned `base`, never longer than `room`.
// When even `base` does not fit the room, base wins: that is the length the boxes always had.
constexpr int boxLength(const int base, const int needed, const int room) {
  return std::max(base, std::min(needed, room));
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
constexpr Stack stacked(const int junction, const int gap, const int base, const int needed, const Span span) {
  const int room = std::min(junction - span.top, span.bottom - (junction + gap));
  const int length = boxLength(base, needed, room);
  return {{junction - length, length}, {junction + gap, length}};
}

// X3: one box on each side edge, level with its button. Grows equally both ways from `centre`.
constexpr Box centred(const int centre, const int base, const int needed, const Span span) {
  const int room = 2 * std::min(centre - span.top, span.bottom - centre);
  const int length = boxLength(base, needed, room);
  return {centre - length / 2, length};
}

}  // namespace SideHintLayout
