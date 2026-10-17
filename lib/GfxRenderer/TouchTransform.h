#pragma once

// Pure geometry for mapping touch coordinates, kept free of Arduino/ESP-IDF so
// it can be host-tested. GfxRenderer::tapToLogical is a thin wrapper over
// tapToLogical() below; the math lives here only so the four-orientation
// behaviour is testable without hardware (see test/touch_transform).
//
// Coordinate contract, as the SDK defines it: InputManager reports taps as
// normalized 0..1 floats in the PANEL's native frame, already corrected for
// swapXY/flipX/flipY by the board profile. This maps that into the LOGICAL
// frame the renderer is currently drawing in.

namespace touchtransform {

// Mirrors GfxRenderer::Orientation's declared order. Kept as plain ints so this
// header needs no GfxRenderer.h (and therefore no Arduino). GfxRenderer.cpp
// static_asserts that the two orders still agree, so drift is a build error
// rather than a silently rotated touch panel.
enum Orientation : int {
  Portrait = 0,
  LandscapeClockwise = 1,
  PortraitInverted = 2,
  LandscapeCounterClockwise = 3,
};

// Panel-native normalized touch -> logical screen pixels. The inverse of
// GfxRenderer's rotateCoordinates().
//
// Clamping happens on the PHYSICAL point, before rotation: a controller that
// reports slightly outside 0..1 (or exactly 1.0, which would land one pixel
// past the edge) is pulled onto the panel first, so every rotation branch is
// guaranteed an in-range input and the logical result is always on screen.
inline void tapToLogical(const int orientation, const int panelWidth, const int panelHeight, const float nx,
                         const float ny, int& outX, int& outY) {
  int phyX = static_cast<int>(nx * panelWidth);
  int phyY = static_cast<int>(ny * panelHeight);
  if (phyX < 0) phyX = 0;
  if (phyX > panelWidth - 1) phyX = panelWidth - 1;
  if (phyY < 0) phyY = 0;
  if (phyY > panelHeight - 1) phyY = panelHeight - 1;

  switch (orientation) {
    case Portrait:
      outX = panelHeight - 1 - phyY;
      outY = phyX;
      break;
    case PortraitInverted:
      outX = phyY;
      outY = panelWidth - 1 - phyX;
      break;
    case LandscapeClockwise:
      outX = panelWidth - 1 - phyX;
      outY = panelHeight - 1 - phyY;
      break;
    case LandscapeCounterClockwise:
    default:
      outX = phyX;
      outY = phyY;
      break;
  }
}

// Logical screen pixels -> panel-native normalized touch: the inverse of tapToLogical(), for code
// that has to produce a touch at a point it knows on screen (the serial CMD:TOUCH). Aims at the
// centre of the physical pixel, so tapToLogical() maps the result back onto the same point.
// The point must be on screen (0 <= x < logical width, 0 <= y < logical height).
inline void logicalToTap(const int orientation, const int panelWidth, const int panelHeight, const int x, const int y,
                         float& outNx, float& outNy) {
  int phyX;
  int phyY;
  switch (orientation) {
    case Portrait:
      phyX = y;
      phyY = panelHeight - 1 - x;
      break;
    case PortraitInverted:
      phyX = panelWidth - 1 - y;
      phyY = x;
      break;
    case LandscapeClockwise:
      phyX = panelWidth - 1 - x;
      phyY = panelHeight - 1 - y;
      break;
    case LandscapeCounterClockwise:
    default:
      phyX = x;
      phyY = y;
      break;
  }
  outNx = (static_cast<float>(phyX) + 0.5f) / static_cast<float>(panelWidth);
  outNy = (static_cast<float>(phyY) + 0.5f) / static_cast<float>(panelHeight);
}

}  // namespace touchtransform
