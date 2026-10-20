#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cmath>
#include <cstdint>

#include "fontIds.h"

// The x4-orbit splash, drawn from primitives (no bitmap in flash): an open book with a tilted
// orbit around it and a small moon on that orbit -- a book on its way in from the library --
// under a sparse field of stars.
namespace {

constexpr float kPi = 3.14159265f;

void fillDisc(const GfxRenderer& r, const int cx, const int cy, const int radius, const bool black) {
  for (int dy = -radius; dy <= radius; ++dy) {
    const int half = static_cast<int>(std::sqrt(static_cast<float>(radius * radius - dy * dy)) + 0.5f);
    r.fillRect(cx - half, cy + dy, half * 2 + 1, 1, black);
  }
}

// Point on an ellipse (semi-axes a, b) rotated by `tilt`, at parameter t.
struct OrbitPoint {
  int x, y;
  bool front;  // the half that passes in front of the book
};

OrbitPoint orbitAt(const int cx, const int cy, const float a, const float b, const float tilt, const float t) {
  const float ex = a * std::cos(t), ey = b * std::sin(t);
  const float ct = std::cos(tilt), st = std::sin(tilt);
  return {cx + static_cast<int>(ex * ct - ey * st), cy + static_cast<int>(ex * st + ey * ct), std::sin(t) > 0.0f};
}

void drawOrbit(const GfxRenderer& r, const int cx, const int cy, const float a, const float b, const float tilt,
               const bool frontHalf) {
  constexpr int STEPS = 360;
  for (int i = 0; i < STEPS; ++i) {
    const auto p = orbitAt(cx, cy, a, b, tilt, 2.0f * kPi * i / STEPS);
    if (p.front != frontHalf) continue;
    // The far side is dashed: it reads as passing behind the book.
    if (!frontHalf && (i / 6) % 2) continue;
    r.fillRect(p.x - 1, p.y - 1, 3, 3, true);
  }
}

// One page of the open book: from the spine out to the fore-edge, the spine end sitting lower
// the way an open book's pages dip into the gutter.
void drawPage(const GfxRenderer& r, const int spineX, const int topY, const int dir, const int w, const int h) {
  constexpr int DIP = 10;
  const int outerX = spineX + dir * w;
  const int xs[] = {spineX, outerX, outerX, spineX};
  const int ys[] = {topY + DIP, topY, topY + h, topY + h + DIP};
  r.fillPolygon(xs, ys, 4, false);  // white over the orbit's far side
  for (int i = 0; i < 3; ++i) r.drawLine(xs[i], ys[i], xs[i + 1], ys[i + 1], 3, true);  // spine edge: caller
  // Lines of text, following the page's slope.
  constexpr int INSET = 14;
  for (int line = 0; line < 6; ++line) {
    const int len = (line == 5) ? w / 2 : w - INSET * 2;
    const int base = topY + 30 + line * 15;
    const int y0 = base + DIP - DIP * INSET / w;
    const int y1 = base + DIP - DIP * (INSET + len) / w;
    r.drawLine(spineX + dir * INSET, y0, spineX + dir * (INSET + len), y1, 2, true);
  }
}

void drawStars(const GfxRenderer& r, const int width, const int height, const int keepOutX0, const int keepOutY0,
               const int keepOutX1, const int keepOutY1) {
  uint32_t seed = 0x6F726269u;  // "orbi": the same sky on every boot
  auto next = [&seed](const uint32_t mod) {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<int>((seed >> 8) % mod);
  };
  for (int i = 0; i < 70; ++i) {
    const int x = next(width - 16) + 8;
    const int y = next(height - 16) + 8;
    const int kind = next(10);
    if (x > keepOutX0 && x < keepOutX1 && y > keepOutY0 && y < keepOutY1) continue;
    if (kind < 6) {
      r.fillRect(x, y, 2, 2, true);
    } else if (kind < 9) {
      r.drawLine(x - 3, y, x + 3, y, true);
      r.drawLine(x, y - 3, x, y + 3, true);
    } else {  // a bright one
      r.drawLine(x - 6, y, x + 6, y, 2, true);
      r.drawLine(x, y - 6, x, y + 6, 2, true);
      r.fillRect(x - 1, y - 1, 3, 3, true);
    }
  }
}

}  // namespace

void BootActivity::onEnter() {
  Activity::onEnter();
  RenderLock lock(*this);

  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  const bool landscape = w > h;

  // Book and orbit a little above centre, so the title sits under them.
  const int cx = w / 2;
  const int cy = h / 2 - (landscape ? 50 : 70);
  constexpr int PAGE_W = 108, PAGE_H = 124;
  const float orbitA = landscape ? 205.0f : 196.0f;
  constexpr float ORBIT_B = 58.0f;
  constexpr float TILT = -0.28f;

  renderer.clearScreen();
  drawStars(renderer, w, h, cx - static_cast<int>(orbitA) - 20, cy - PAGE_H, cx + static_cast<int>(orbitA) + 20,
            cy + PAGE_H + 120);

  drawOrbit(renderer, cx, cy, orbitA, ORBIT_B, TILT, /*frontHalf=*/false);

  const int topY = cy - PAGE_H / 2 - 5;
  drawPage(renderer, cx, topY, -1, PAGE_W, PAGE_H);
  drawPage(renderer, cx, topY, +1, PAGE_W, PAGE_H);
  renderer.drawLine(cx, topY + 10, cx, topY + PAGE_H + 10, 3, true);  // spine
  {
    // Bookmark ribbon hanging from the right page's top edge.
    const int rx = cx + PAGE_W - 30;
    const int xs[] = {rx, rx + 12, rx + 12, rx + 6, rx};
    const int ys[] = {topY + 4, topY + 3, topY + 40, topY + 32, topY + 41};
    renderer.fillPolygon(xs, ys, 5, true);
  }

  drawOrbit(renderer, cx, cy, orbitA, ORBIT_B, TILT, /*frontHalf=*/true);

  // The moon on the near side, with a short trail behind it.
  const float moonT = 0.28f * kPi;
  for (int k = 3; k >= 1; --k) {
    const auto p = orbitAt(cx, cy, orbitA, ORBIT_B, TILT, moonT - 0.11f * k);
    fillDisc(renderer, p.x, p.y, 5 - k, true);
  }
  const auto moon = orbitAt(cx, cy, orbitA, ORBIT_B, TILT, moonT);
  fillDisc(renderer, moon.x, moon.y, 14, false);  // a white halo cuts the orbit line
  fillDisc(renderer, moon.x, moon.y, 10, true);
  fillDisc(renderer, moon.x - 3, moon.y - 3, 3, false);  // highlight

  const int titleY = cy + PAGE_H / 2 + 78;
  constexpr const char* TITLE = "x4 orbit";
  constexpr float TITLE_SCALE = 1.7f;
  const int titleW = renderer.getTextWidthScaled(UI_12_FONT_ID, TITLE, EpdFontFamily::BOLD, TITLE_SCALE);
  renderer.drawTextScaled(UI_12_FONT_ID, cx - titleW / 2, titleY - 14, TITLE, true, EpdFontFamily::BOLD, TITLE_SCALE);
  // A short rule under the title, broken by a dot.
  const int ruleY = titleY - 14 + renderer.getLineHeightScaled(UI_12_FONT_ID, TITLE_SCALE) + 4;
  renderer.drawLine(cx - 70, ruleY, cx - 10, ruleY, 2, true);
  renderer.drawLine(cx + 10, ruleY, cx + 70, ruleY, 2, true);
  renderer.fillRect(cx - 2, ruleY - 2, 5, 5, true);
  renderer.drawCenteredText(SMALL_FONT_ID, ruleY + 12, "your library, in orbit");

  renderer.drawCenteredText(SMALL_FONT_ID, h - 56, tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, h - 30, CROSSPOINT_VERSION);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
