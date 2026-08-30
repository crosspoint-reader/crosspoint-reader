#pragma once

// Pure geometry for the Settings category landing screen: the grid of
// icon + full-name cards that replaces the tab band on touch boards, where six
// categories across one band truncate every label to a few characters.
//
// Header-only, and free of GfxRenderer/FreeInkUI/HAL, so a host gtest can
// exercise it directly -- SettingsActivity itself needs renderer and input
// mocks the test harness does not have.
//
// cellRect() and cellHitPadding() are the ONLY geometry in the feature.
// SettingsActivity hands each card's cellRect() straight to fui::button() as
// its draw rect and cellHitPadding() as its ButtonProps::hitPadding, so the
// rect FreeInkUI registers in the interaction table is exactly the rect that
// was drawn, expanded by exactly that padding. hitTest() below is written in
// terms of those same two functions rather than restating the arithmetic, so
// the hit model and the drawn layout cannot drift apart. (The SDK applies one
// further step to the registered rect, ensureMinTouchRect()'s screen-edge
// snap, which can only grow the outermost cards toward the panel edge -- it
// never moves a boundary between two cards.)
namespace SettingsCategoryGridLayout {

struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;

  [[nodiscard]] constexpr int right() const { return x + width; }
  [[nodiscard]] constexpr int bottom() const { return y + height; }
  // Half-open on the right/bottom edge, matching freeink::ui::Rect::contains,
  // so cards that share a boundary pixel cannot both claim a tap.
  [[nodiscard]] constexpr bool contains(const int px, const int py) const {
    return px >= x && py >= y && px < right() && py < bottom();
  }
};

// CSS order (top, right, bottom, left), matching freeink::ui::Insets so the
// value can be copied field-for-field into ButtonProps::hitPadding.
struct Insets {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
};

// Sizing inputs, supplied by the activity from the active theme so the grid
// tracks UI scale; nothing here assumes a panel size or an orientation.
struct Metrics {
  int gap = 12;  // space between cards, split between their hit zones
  // Card width the column count aims for: the band's width is divided by this
  // and rounded to the nearest whole number of columns.
  int targetCellWidth = 240;
  int minCellWidth = 96;
  int minCellHeight = 44;  // FreeInkUI's minimum touch target
  int maxCellHeight = 0;   // 0 = cards fill the band's height
  int maxColumns = 3;
};

struct Plan {
  int columns = 0;
  int rows = 0;
  int x = 0;  // grid origin (the band's leftover space is split evenly)
  int y = 0;
  int cellWidth = 0;
  int cellHeight = 0;
  int gap = 0;
  int count = 0;

  [[nodiscard]] constexpr bool valid() const {
    return count > 0 && columns > 0 && rows > 0 && cellWidth > 0 && cellHeight > 0;
  }
  [[nodiscard]] constexpr int gridWidth() const { return columns * cellWidth + gap * (columns - 1); }
  [[nodiscard]] constexpr int gridHeight() const { return rows * cellHeight + gap * (rows - 1); }
};

constexpr int ceilDiv(const int value, const int divisor) {
  return divisor <= 0 ? 0 : (value + divisor - 1) / divisor;
}

// Extent of one lane when `total` is split into `lanes` lanes separated by
// `gap`. Floors, so lanes * extent plus the gaps never exceeds total.
constexpr int laneExtent(const int total, const int lanes, const int gap) {
  return lanes <= 0 ? 0 : (total - gap * (lanes - 1)) / lanes;
}

// Self-inverse: applied twice it returns the original column, so one function
// maps logical to visual (render) and visual to logical (hit-test).
constexpr int mirroredColumn(const int col, const int columns, const bool rtl) {
  return rtl ? columns - 1 - col : col;
}

// Number of columns the band's width asks for, before the height check in
// plan() gets a say. Rounds to the nearest whole number of target-width cards,
// then gives up columns until each one clears minCellWidth.
inline int columnsForWidth(const int bandWidth, const int count, const Metrics& m) {
  if (bandWidth <= 0 || count <= 0) return 0;
  const int gap = m.gap > 0 ? m.gap : 0;
  const int target = m.targetCellWidth > 0 ? m.targetCellWidth : 1;
  int columns = (bandWidth + target / 2) / target;
  if (columns < 1) columns = 1;
  if (columns > m.maxColumns) columns = m.maxColumns;
  if (columns > count) columns = count;
  while (columns > 1 && laneExtent(bandWidth, columns, gap) < m.minCellWidth) columns--;
  return columns;
}

// Lays `count` cards out inside `band` (the content area the activity already
// reserved below the header and above the button hints), so this stays
// orientation-agnostic: it only ever sees the band it is given.
inline Plan plan(const Rect& band, const int count, const Metrics& m) {
  Plan p;
  if (count <= 0 || band.width <= 0 || band.height <= 0) return p;

  const int gap = m.gap > 0 ? m.gap : 0;
  const int maxColumns = m.maxColumns < count ? m.maxColumns : count;
  int columns = columnsForWidth(band.width, count, m);
  if (columns <= 0) return p;

  // Trade card width for card height: one more column removes a row, which is
  // what buys the cards back their minimum touch height on a short band. Stops
  // as soon as a further column would push the width below its own floor.
  while (laneExtent(band.height, ceilDiv(count, columns), gap) < m.minCellHeight && columns < maxColumns &&
         laneExtent(band.width, columns + 1, gap) >= m.minCellWidth) {
    columns++;
  }

  p.columns = columns;
  p.rows = ceilDiv(count, columns);
  p.gap = gap;
  p.count = count;
  p.cellWidth = laneExtent(band.width, p.columns, gap);
  p.cellHeight = laneExtent(band.height, p.rows, gap);
  if (m.maxCellHeight > 0 && p.cellHeight > m.maxCellHeight) p.cellHeight = m.maxCellHeight;
  if (!p.valid()) return Plan{};

  p.x = band.x + (band.width - p.gridWidth()) / 2;
  p.y = band.y + (band.height - p.gridHeight()) / 2;
  return p;
}

// The drawn rect of one card. The renderer passes this straight to
// fui::button(); nothing else computes a card's position.
inline Rect cellRect(const Plan& p, const int index, const bool rtl = false) {
  if (!p.valid() || index < 0 || index >= p.count) return Rect{};
  const int row = index / p.columns;
  const int col = mirroredColumn(index % p.columns, p.columns, rtl);
  return Rect{p.x + col * (p.cellWidth + p.gap), p.y + row * (p.cellHeight + p.gap), p.cellWidth, p.cellHeight};
}

// Hit-zone growth applied to every card, passed to ButtonProps::hitPadding.
// Neighbours split the gutter between them, so a tap that lands in the space
// between two cards reaches the nearer one instead of nothing. An even gap
// tiles exactly; an odd gap leaves its single middle pixel line unclaimed.
inline Insets cellHitPadding(const Plan& p) {
  const int half = p.gap / 2;
  return Insets{half, half, half, half};
}

inline Rect padded(const Rect& r, const Insets& pad) {
  return Rect{r.x - pad.left, r.y - pad.top, r.width + pad.left + pad.right, r.height + pad.top + pad.bottom};
}

// Which card a tap lands on, or -1 for a miss. Composed from cellRect() and
// cellHitPadding() -- the same values the renderer registers -- rather than
// re-deriving row/column arithmetic from the coordinates.
inline int hitTest(const Plan& p, const int px, const int py, const bool rtl = false) {
  if (!p.valid()) return -1;
  const Insets pad = cellHitPadding(p);
  for (int i = 0; i < p.count; i++) {
    if (padded(cellRect(p, i, rtl), pad).contains(px, py)) return i;
  }
  return -1;
}

}  // namespace SettingsCategoryGridLayout
