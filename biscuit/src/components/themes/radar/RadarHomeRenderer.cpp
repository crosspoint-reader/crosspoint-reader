#include "RadarHomeRenderer.h"

#include <GfxRenderer.h>
#include <EpdFontFamily.h>

#include "fontIds.h"

// ---------------------------------------------------------------------------
// Node position table — 8 unit-vector pairs scaled by 1000, clockwise from top.
// Index 0 = top, 1 = top-right, 2 = right, 3 = bottom-right,
//       4 = bottom, 5 = bottom-left, 6 = left, 7 = top-left.
// Values are sin/cos * 1000, integer only (no <cmath>).
// ---------------------------------------------------------------------------
static const int NODE_DX[8] = {    0,  707, 1000,  707,    0, -707, -1000, -707 };
static const int NODE_DY[8] = { -1000, -707,    0,  707, 1000,  707,     0, -707 };

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void RadarHomeRenderer::drawCircle(GfxRenderer& r, int cx, int cy, int radius) {
  // Bresenham midpoint circle, draw 8-symmetrically
  int x = 0;
  int y = radius;
  int d = 1 - radius;

  auto plot8 = [&](int px, int py) {
    r.drawPixel(cx + px, cy + py);
    r.drawPixel(cx - px, cy + py);
    r.drawPixel(cx + px, cy - py);
    r.drawPixel(cx - px, cy - py);
    r.drawPixel(cx + py, cy + px);
    r.drawPixel(cx - py, cy + px);
    r.drawPixel(cx + py, cy - px);
    r.drawPixel(cx - py, cy - px);
  };

  while (x <= y) {
    plot8(x, y);
    if (d < 0) {
      d += 2 * x + 3;
    } else {
      d += 2 * (x - y) + 5;
      y--;
    }
    x++;
  }
}

void RadarHomeRenderer::drawDashedLine(GfxRenderer& r, int x1, int y1, int x2, int y2, int dashLen, int gapLen) {
  const int dx = x2 - x1;
  const int dy = y2 - y1;
  const int adx = dx < 0 ? -dx : dx;
  const int ady = dy < 0 ? -dy : dy;
  const int steps = adx > ady ? adx : ady;
  if (steps <= 0) return;
  const int period = dashLen + gapLen;
  // Fixed-point step (multiply, not shift — signed-shift is impl-defined)
  const int sx = (dx * 256) / steps;
  const int sy = (dy * 256) / steps;
  int fx = x1 * 256;
  int fy = y1 * 256;
  for (int i = 0; i <= steps; ++i) {
    if ((i % period) < dashLen) {
      const int px = fx / 256;
      const int py = fy / 256;
      r.drawPixel(px, py);
    }
    fx += sx;
    fy += sy;
  }
}

void RadarHomeRenderer::drawRoundRectOutline(GfxRenderer& r, int x, int y, int w, int h, int radius) {
  // Use the built-in renderer method which already exists
  r.drawRoundedRect(x, y, w, h, 1, radius, true);
}

void RadarHomeRenderer::fillRoundRect(GfxRenderer& r, int x, int y, int w, int h, int radius) {
  // Use bool state=true (theme-aware foreground) rather than Color::Black so
  // the fill polarity follows the renderer's active theme instead of being
  // hard-wired to a literal colour.
  r.fillRect(x, y, w, h, true);
}

void RadarHomeRenderer::drawTextCenteredInRect(GfxRenderer& r, int rx, int ry, int rw, int rh,
                                               const char* text, bool blackText) {
  // Pass BOLD to getTextWidth so centering math matches the bold glyphs drawn below.
  int tw = r.getTextWidth(SMALL_FONT_ID, text, EpdFontFamily::BOLD);
  int th = r.getTextHeight(SMALL_FONT_ID);
  int tx = rx + (rw - tw) / 2;
  int ty = ry + (rh - th) / 2;
  r.drawText(SMALL_FONT_ID, tx, ty, text, blackText, EpdFontFamily::BOLD);
}

// ---------------------------------------------------------------------------
// Main draw entry point
// ---------------------------------------------------------------------------

void RadarHomeRenderer::draw(GfxRenderer& renderer, const RadarNode* nodes, int selectedIndex,
                             const RadarHomeStatus& status) {
  const int pageWidth = renderer.getScreenWidth();   // 480

  // -----------------------------------------------------------------------
  // A. Top bar: "BISCUIT." brand left, battery right, then separator line
  // -----------------------------------------------------------------------
  renderer.drawText(UI_12_FONT_ID, 14, 8, "biscuit.", true, EpdFontFamily::BOLD);

  // Battery percent string at top-right
  char batStr[12];
  if (status.batteryPercent >= 0 && status.batteryPercent <= 100) {
    snprintf(batStr, sizeof(batStr), "%d%%", status.batteryPercent);
  } else {
    batStr[0] = '\0';
  }
  if (batStr[0] != '\0') {
    int batW = renderer.getTextWidth(SMALL_FONT_ID, batStr);
    renderer.drawText(SMALL_FONT_ID, pageWidth - 14 - batW, 12, batStr, true);
  }

  // Separator at y=35
  renderer.drawLine(14, 35, pageWidth - 14, 35, true);

  // -----------------------------------------------------------------------
  // B. Three concentric circles around center
  // -----------------------------------------------------------------------
  drawCircle(renderer, CENTER_X, CENTER_Y, RADIUS_OUTER);
  drawCircle(renderer, CENTER_X, CENTER_Y, RADIUS_MID);
  drawCircle(renderer, CENTER_X, CENTER_Y, RADIUS_INNER);

  // -----------------------------------------------------------------------
  // C. Crosshair lines through center
  // -----------------------------------------------------------------------
  // Horizontal and vertical lines clipped to outer circle
  renderer.drawLine(CENTER_X - RADIUS_OUTER, CENTER_Y, CENTER_X + RADIUS_OUTER, CENTER_Y, true);
  renderer.drawLine(CENTER_X, CENTER_Y - RADIUS_OUTER, CENTER_X, CENTER_Y + RADIUS_OUTER, true);

  // Diagonal lines: 45 degrees. sin(45)*190 ≈ 134 (190/sqrt(2) ≈ 134)
  constexpr int DIAG = 134;
  renderer.drawLine(CENTER_X - DIAG, CENTER_Y - DIAG, CENTER_X + DIAG, CENTER_Y + DIAG, true);
  renderer.drawLine(CENTER_X + DIAG, CENTER_Y - DIAG, CENTER_X - DIAG, CENTER_Y + DIAG, true);

  // -----------------------------------------------------------------------
  // D. Nodes + connectors
  // -----------------------------------------------------------------------
  for (int i = 0; i < NODE_COUNT; i++) {
    int nodeCX = CENTER_X + NODE_DX[i] * NODE_RING_RADIUS / 1000;
    int nodeCY = CENTER_Y + NODE_DY[i] * NODE_RING_RADIUS / 1000;

    int nodeX = nodeCX - NODE_W / 2;
    int nodeY = nodeCY - NODE_H / 2;

    // Connector from center to node center
    if (i == selectedIndex) {
      // Solid double-line connector for selected node
      renderer.drawLine(CENTER_X, CENTER_Y, nodeCX, nodeCY, true);
      renderer.drawLine(CENTER_X + 1, CENTER_Y + 1, nodeCX + 1, nodeCY + 1, true);
    } else {
      drawDashedLine(renderer, CENTER_X, CENTER_Y, nodeCX, nodeCY, 5, 4);
    }

    // Node box — square (sharp corners), opaque fill so radar lines don't bleed through
    if (i == selectedIndex) {
      // Selected: filled (foreground) box with inverted text
      renderer.fillRect(nodeX, nodeY, NODE_W, NODE_H, true);
      drawTextCenteredInRect(renderer, nodeX, nodeY, NODE_W, NODE_H, nodes[i].label, false);
    } else {
      // Unselected: opaque background fill covers lines behind, then sharp outline + text
      renderer.fillRect(nodeX, nodeY, NODE_W, NODE_H, false);
      renderer.drawRect(nodeX, nodeY, NODE_W, NODE_H, true);
      drawTextCenteredInRect(renderer, nodeX, nodeY, NODE_W, NODE_H, nodes[i].label, true);
    }
  }

  // -----------------------------------------------------------------------
  // E. Status area below the radar circle
  // -----------------------------------------------------------------------
  constexpr int statusTopY = CENTER_Y + RADIUS_OUTER + 18;  // ~498 + gap

  // Horizontal rule
  renderer.drawLine(14, statusTopY, pageWidth - 14, statusTopY, true);

  // "RADIO" label + value
  renderer.drawText(SMALL_FONT_ID, 14, statusTopY + 8, "RADIO", true, EpdFontFamily::BOLD);
  if (status.radioLeft != nullptr) {
    renderer.drawText(SMALL_FONT_ID, 14, statusTopY + 22, status.radioLeft, true);
  }

  // "SYSTEM" label + value (right column)
  constexpr int rightColX = 260;
  renderer.drawText(SMALL_FONT_ID, rightColX, statusTopY + 8, "SYSTEM", true, EpdFontFamily::BOLD);
  if (status.systemRight != nullptr) {
    renderer.drawText(SMALL_FONT_ID, rightColX, statusTopY + 22, status.systemRight, true);
  }

  // Battery bar: outline rect then filled portion
  constexpr int barY = statusTopY + 46;
  constexpr int barX = 14;
  const int barW = pageWidth - 28;
  constexpr int barH = 8;
  renderer.drawRect(barX, barY, barW, barH, true);
  if (status.batteryPercent > 0) {
    int fillW = (barW - 2) * status.batteryPercent / 100;
    if (fillW > 0) {
      renderer.fillRect(barX + 1, barY + 1, fillW, barH - 2, true);
    }
  }
}
