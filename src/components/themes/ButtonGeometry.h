#pragma once

#include <BoardConfig.h>

// Where each physical button actually sits on the device, so a hint can be drawn
// next to the key it describes instead of in a row that assumes every board has
// the X3/X4 four-button strip.
//
// Why this lives here and not in the SDK: BoardProfile models what the hardware
// can DO — pins, controllers, capabilities. This is where a key sits in a *case*,
// which is the same category as ViewableInsets (bezel overlap) and arguably
// belongs alongside it. Worth proposing upstream once the shape has settled; kept
// firmware-side for now because it is only consumed by the theme layer.
//
// Positions are fractions rather than pixels because panels differ (480/528/540
// wide in portrait) while the physical placement is fixed in millimetres. Given
// in the PORTRAIT frame; the themes rotate as needed.
namespace ButtonGeometry {

enum class Edge : uint8_t { None, Left, Right, Bottom };

struct Anchor {
  Edge edge = Edge::None;
  // Along the edge: measured from the BOTTOM for Left/Right, from the LEFT for
  // Bottom. 0.5 is centred. Ignored when edge == None.
  float fraction = 0.0f;

  constexpr bool present() const { return edge != Edge::None; }
};

struct Layout {
  Anchor up;       // physical Up key, if any
  Anchor down;     // physical Down key
  Anchor confirm;  // capacitive home key, where the board has one
  Anchor power;

  // True for the X3/X4 family, whose hints keep their historic hand-tuned
  // placement: a four-box strip along the bottom plus side Up/Down. Those layouts
  // were measured on hardware and both boards are validated, so they are left
  // exactly as they were rather than re-derived from fractions.
  bool legacyStrip = false;
};

// Measured on hardware 2026-08-17. The T5S3's figures come from a 10 cm portrait
// screen height: BOOT 4 cm from the bottom -> 0.40, IO48 2.8 cm -> 0.28.
inline Layout forActiveBoard() {
  Layout l;
  switch (BoardConfig::ACTIVE.board) {
    case BoardConfig::Board::LilyGoT5S3:
      // Two physical keys, both on the left edge, plus the capacitive home key
      // centred under the panel. There is no Up key — it is synthesised by
      // holding Down (see HalGPIO::downHoldDrivesUp_), so `up` stays absent and
      // the Down hint has to carry both roles.
      l.down = {Edge::Left, 0.28f};
      l.power = {Edge::Left, 0.40f};
      l.confirm = {Edge::Bottom, 0.5f};
      break;

    case BoardConfig::Board::XteinkX4Pro:
      // Up/Down sit on the sides as on the X3, and as high up (confirmed on
      // the device 2026-10-12; this was 0.5, which put them at mid-height); no
      // Left/Right keys at all. The capacitive home key is centred on the
      // lower screen edge.
      l.up = {Edge::Left, 0.75f};
      l.down = {Edge::Right, 0.75f};
      l.confirm = {Edge::Bottom, 0.5f};
      l.legacyStrip = true;
      break;

    case BoardConfig::Board::XteinkX3:
    case BoardConfig::Board::XteinkX3Uc8279:
      // Up on the left edge, Down on the right, level with each other. The
      // height is read off the hand-tuned hint boxes (155-233 px of 792),
      // which the hints keep (legacyStrip); recorded so sideKeysOnOppositeEdges()
      // can answer from the layout rather than from the board's name.
      l.up = {Edge::Left, 0.75f};
      l.down = {Edge::Right, 0.75f};
      l.legacyStrip = true;
      break;

    case BoardConfig::Board::XteinkX4:
      l.legacyStrip = true;
      break;

    default:
      // Unmeasured board: no anchors, so the theme falls back to an evenly
      // spread strip rather than inventing placements.
      break;
  }
  return l;
}

// Up and Down sit on opposite side edges -- Up on the left, Down on the right --
// as on the X3 and the X4 Pro, rather than one above the other on the right as on
// the X4. The side hints then draw one box on each edge, and getContentRect()
// reserves a column on both.
//
// Asked of the layout, not of the board's name: deviceIsX3() answered it for the
// X3 alone, so the X4 Pro got the X4's stacked boxes on the wrong edge.
inline bool sideKeysOnOppositeEdges() {
  const Layout l = forActiveBoard();
  return l.up.edge == Edge::Left && l.down.edge == Edge::Right;
}

}  // namespace ButtonGeometry
