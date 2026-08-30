#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "activities/settings/SettingsCategoryGridLayout.h"

namespace grid = SettingsCategoryGridLayout;

namespace {

// The metrics SettingsActivity::landingMetrics() derives from the default
// FreeInkUI ThemeTokens (spaceMd 8, rowHeight 44, minTouchSize 44). These fixed
// numbers live HERE and nowhere else: the layout header deliberately carries no
// defaults, so that a caller cannot lay the grid out at some constant scale
// instead of the active theme's. This fixture stands in for the theme.
grid::Metrics defaultMetrics() {
  grid::Metrics m;
  m.gap = 8;
  m.targetCellWidth = 44 * 5;  // 220
  m.minCellWidth = 44 * 2;     // 88
  m.minCellHeight = 44;
  m.maxCellHeight = 44 * 4;  // 176
  m.maxColumns = 3;
  return m;
}

// The content band SettingsActivity hands the grid: the screen minus the
// header band it reserved with setContentMargin(). The four orientations of an
// 800x480 panel produce two distinct band shapes (portrait and its 180 flip
// are the same band; the two landscape rotations likewise), and the layout only
// ever sees the band -- it never reads a screen size or a rotation.
constexpr int HEADER = 48;

grid::Rect bandFor(const int screenWidth, const int screenHeight) {
  return grid::Rect{0, HEADER, screenWidth, screenHeight - HEADER};
}

struct Orientation {
  const char* name;
  int width;
  int height;
};

const std::vector<Orientation> ORIENTATIONS = {
    {"Portrait", 480, 800}, {"InvertedPortrait", 480, 800}, {"LandscapeCW", 800, 480}, {"LandscapeCCW", 800, 480}};

bool overlaps(const grid::Rect& a, const grid::Rect& b) {
  return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

bool encloses(const grid::Rect& outer, const grid::Rect& inner) {
  return inner.x >= outer.x && inner.y >= outer.y && inner.right() <= outer.right() && inner.bottom() <= outer.bottom();
}

}  // namespace

// --- Metrics carries no layout defaults ------------------------------------

TEST(SettingsCategoryGridLayout, DefaultConstructedMetricsIsNotUsable) {
  // The header must not ship fixed sizing of its own: a caller that forgets to
  // read the active theme has to get nothing, not a grid laid out at some
  // constant scale that ignores UITheme.
  const grid::Metrics bare;
  EXPECT_FALSE(bare.populated());
  EXPECT_EQ(bare.targetCellWidth, 0);
  EXPECT_EQ(bare.minCellWidth, 0);
  EXPECT_EQ(bare.minCellHeight, 0);
  EXPECT_EQ(bare.maxColumns, 0);

  EXPECT_EQ(grid::columnsForWidth(800, 6, bare), 0);
  const grid::Plan p = grid::plan(bandFor(800, 480), 6, bare);
  EXPECT_FALSE(p.valid());
  EXPECT_EQ(grid::hitTest(p, 400, 240), -1);
  EXPECT_EQ(grid::cellRect(p, 0).width, 0);
}

TEST(SettingsCategoryGridLayout, ThemeSuppliedMetricsIsUsable) {
  const grid::Metrics m = defaultMetrics();
  EXPECT_TRUE(m.populated());
  EXPECT_TRUE(grid::plan(bandFor(800, 480), 6, m).valid());
}

TEST(SettingsCategoryGridLayout, EveryScaleMetricIsRequired) {
  // Dropping any one of the four scale-bearing fields makes the whole Metrics
  // unusable, so a partially-populated one cannot half-follow the theme.
  // gap and maxCellHeight are excluded on purpose: 0 means "no gutter" and
  // "no height ceiling", both legitimate.
  for (int field = 0; field < 4; field++) {
    grid::Metrics m = defaultMetrics();
    switch (field) {
      case 0:
        m.targetCellWidth = 0;
        break;
      case 1:
        m.minCellWidth = 0;
        break;
      case 2:
        m.minCellHeight = 0;
        break;
      default:
        m.maxColumns = 0;
        break;
    }
    EXPECT_FALSE(m.populated()) << "field " << field;
    EXPECT_FALSE(grid::plan(bandFor(800, 480), 6, m).valid()) << "field " << field;
  }

  grid::Metrics zeroGap = defaultMetrics();
  zeroGap.gap = 0;
  zeroGap.maxCellHeight = 0;
  EXPECT_TRUE(zeroGap.populated());
  EXPECT_TRUE(grid::plan(bandFor(800, 480), 6, zeroGap).valid());
}

// --- plan(): column and row choice -----------------------------------------

TEST(SettingsCategoryGridLayout, EmptyCategoryListProducesNoGrid) {
  const grid::Plan p = grid::plan(bandFor(480, 800), 0, defaultMetrics());
  EXPECT_FALSE(p.valid());
  EXPECT_EQ(p.count, 0);
  EXPECT_EQ(p.columns, 0);
  EXPECT_EQ(p.rows, 0);
  // Nothing is drawable and nothing is hittable.
  EXPECT_EQ(grid::hitTest(p, 240, 400), -1);
  EXPECT_EQ(grid::cellRect(p, 0).width, 0);
}

TEST(SettingsCategoryGridLayout, DegenerateBandProducesNoGrid) {
  const grid::Metrics m = defaultMetrics();
  EXPECT_FALSE(grid::plan(grid::Rect{0, 0, 0, 800}, 6, m).valid());
  EXPECT_FALSE(grid::plan(grid::Rect{0, 0, 480, 0}, 6, m).valid());
  EXPECT_FALSE(grid::plan(grid::Rect{0, 0, 480, -10}, 6, m).valid());
}

TEST(SettingsCategoryGridLayout, ShortCategoryListNeverExceedsItsCount) {
  const grid::Metrics m = defaultMetrics();
  for (int count = 1; count <= 3; count++) {
    const grid::Plan p = grid::plan(bandFor(800, 480), count, m);
    ASSERT_TRUE(p.valid()) << "count=" << count;
    EXPECT_LE(p.columns, count) << "count=" << count;
    EXPECT_EQ(p.rows, grid::ceilDiv(count, p.columns)) << "count=" << count;
    EXPECT_EQ(p.count, count);
  }
}

TEST(SettingsCategoryGridLayout, PortraitPutsSixCategoriesInTwoColumns) {
  const grid::Plan p = grid::plan(bandFor(480, 800), 6, defaultMetrics());
  ASSERT_TRUE(p.valid());
  EXPECT_EQ(p.columns, 2);
  EXPECT_EQ(p.rows, 3);
}

TEST(SettingsCategoryGridLayout, LandscapePutsSixCategoriesInThreeColumns) {
  const grid::Plan p = grid::plan(bandFor(800, 480), 6, defaultMetrics());
  ASSERT_TRUE(p.valid());
  EXPECT_EQ(p.columns, 3);
  EXPECT_EQ(p.rows, 2);
}

TEST(SettingsCategoryGridLayout, FourBuiltInCategoriesFillEveryOrientation) {
  const grid::Metrics m = defaultMetrics();
  for (const auto& o : ORIENTATIONS) {
    const grid::Rect band = bandFor(o.width, o.height);
    const grid::Plan p = grid::plan(band, 4, m);
    ASSERT_TRUE(p.valid()) << o.name;
    EXPECT_GE(p.cellWidth, m.minCellWidth) << o.name;
    EXPECT_GE(p.cellHeight, m.minCellHeight) << o.name;
    EXPECT_LE(p.cellHeight, m.maxCellHeight) << o.name;
  }
}

TEST(SettingsCategoryGridLayout, ExtraColumnsBuyBackTheMinimumTouchHeight) {
  grid::Metrics m = defaultMetrics();
  m.maxCellHeight = 0;
  // A short band: one column would give six 20px rows, below the 44px floor.
  const grid::Plan p = grid::plan(grid::Rect{0, 0, 700, 140}, 6, m);
  ASSERT_TRUE(p.valid());
  EXPECT_EQ(p.rows, 2);
  EXPECT_GE(p.cellHeight, m.minCellHeight);
  EXPECT_GE(p.cellWidth, m.minCellWidth);
}

TEST(SettingsCategoryGridLayout, NarrowBandFallsBackToOneColumn) {
  const grid::Plan p = grid::plan(grid::Rect{0, 0, 120, 600}, 6, defaultMetrics());
  ASSERT_TRUE(p.valid());
  EXPECT_EQ(p.columns, 1);
  EXPECT_EQ(p.rows, 6);
}

TEST(SettingsCategoryGridLayout, MaxCellHeightCapsAndRecentresTheGrid) {
  const grid::Metrics m = defaultMetrics();
  const grid::Rect band = bandFor(480, 800);
  const grid::Plan p = grid::plan(band, 6, m);
  ASSERT_TRUE(p.valid());
  EXPECT_EQ(p.cellHeight, m.maxCellHeight);
  // Leftover vertical space is split evenly above and below the grid.
  const int above = p.y - band.y;
  const int below = band.bottom() - (p.y + p.gridHeight());
  EXPECT_LE(std::abs(above - below), 1);
}

// --- cellRect(): the geometry the renderer draws ----------------------------

TEST(SettingsCategoryGridLayout, CellsAreUniformInsideTheBandAndNeverOverlap) {
  const grid::Metrics m = defaultMetrics();
  for (const auto& o : ORIENTATIONS) {
    const grid::Rect band = bandFor(o.width, o.height);
    const grid::Plan p = grid::plan(band, 6, m);
    ASSERT_TRUE(p.valid()) << o.name;

    std::vector<grid::Rect> cells;
    cells.reserve(static_cast<size_t>(p.count));
    for (int i = 0; i < p.count; i++) cells.push_back(grid::cellRect(p, i));

    for (const auto& c : cells) {
      EXPECT_EQ(c.width, p.cellWidth) << o.name;
      EXPECT_EQ(c.height, p.cellHeight) << o.name;
      EXPECT_TRUE(encloses(band, c)) << o.name << " cell escaped the band";
    }
    for (size_t i = 0; i < cells.size(); i++) {
      for (size_t j = i + 1; j < cells.size(); j++) {
        EXPECT_FALSE(overlaps(cells[i], cells[j])) << o.name << " cells " << i << "," << j;
      }
    }
  }
}

TEST(SettingsCategoryGridLayout, CellsAdvanceInReadingOrder) {
  const grid::Plan p = grid::plan(bandFor(800, 480), 6, defaultMetrics());
  ASSERT_TRUE(p.valid());
  ASSERT_EQ(p.columns, 3);
  // Same row: strictly to the right, one gap apart.
  EXPECT_EQ(grid::cellRect(p, 1).x - grid::cellRect(p, 0).x, p.cellWidth + p.gap);
  EXPECT_EQ(grid::cellRect(p, 1).y, grid::cellRect(p, 0).y);
  // Next row: same column, one row down.
  EXPECT_EQ(grid::cellRect(p, 3).x, grid::cellRect(p, 0).x);
  EXPECT_EQ(grid::cellRect(p, 3).y - grid::cellRect(p, 0).y, p.cellHeight + p.gap);
}

TEST(SettingsCategoryGridLayout, OutOfRangeIndexHasNoRect) {
  const grid::Plan p = grid::plan(bandFor(800, 480), 6, defaultMetrics());
  ASSERT_TRUE(p.valid());
  EXPECT_EQ(grid::cellRect(p, -1).width, 0);
  EXPECT_EQ(grid::cellRect(p, 6).width, 0);
}

TEST(SettingsCategoryGridLayout, PartialLastRowKeepsFullSizeCells) {
  // Five categories over three columns: the last row holds two.
  const grid::Plan p = grid::plan(bandFor(800, 480), 5, defaultMetrics());
  ASSERT_TRUE(p.valid());
  ASSERT_EQ(p.columns, 3);
  ASSERT_EQ(p.rows, 2);
  EXPECT_EQ(grid::cellRect(p, 4).width, p.cellWidth);
  EXPECT_EQ(grid::cellRect(p, 4).y, grid::cellRect(p, 3).y);
  EXPECT_EQ(grid::cellRect(p, 5).width, 0);  // the empty third slot is not a cell
}

// --- hitTest(): built from cellRect() + cellHitPadding() --------------------

TEST(SettingsCategoryGridLayout, EveryCellCentreHitsItsOwnCard) {
  const grid::Metrics m = defaultMetrics();
  for (const auto& o : ORIENTATIONS) {
    const grid::Plan p = grid::plan(bandFor(o.width, o.height), 6, m);
    ASSERT_TRUE(p.valid()) << o.name;
    for (int i = 0; i < p.count; i++) {
      const grid::Rect c = grid::cellRect(p, i);
      EXPECT_EQ(grid::hitTest(p, c.x + c.width / 2, c.y + c.height / 2), i) << o.name << " cell " << i;
    }
  }
}

TEST(SettingsCategoryGridLayout, CellCornersHitAndTheExclusiveEdgeDoesNot) {
  const grid::Plan p = grid::plan(bandFor(800, 480), 6, defaultMetrics());
  ASSERT_TRUE(p.valid());
  const grid::Rect c = grid::cellRect(p, 0);
  EXPECT_EQ(grid::hitTest(p, c.x, c.y), 0);                       // inclusive top-left
  EXPECT_EQ(grid::hitTest(p, c.right() - 1, c.bottom() - 1), 0);  // inclusive last pixel
  EXPECT_NE(grid::hitTest(p, c.x - 1 - p.gap / 2, c.y), 0);       // past the padded left edge
}

TEST(SettingsCategoryGridLayout, GutterTapsReachTheNearerCardInsteadOfNothing) {
  const grid::Plan p = grid::plan(bandFor(800, 480), 6, defaultMetrics());
  ASSERT_TRUE(p.valid());
  ASSERT_EQ(p.gap % 2, 0) << "the even-gap tiling assumption below";
  const grid::Rect a = grid::cellRect(p, 0);
  const grid::Rect b = grid::cellRect(p, 1);
  const int y = a.y + a.height / 2;

  // Every pixel column of the vertical gutter belongs to one of the two cards.
  for (int x = a.right(); x < b.x; x++) {
    const int hit = grid::hitTest(p, x, y);
    EXPECT_TRUE(hit == 0 || hit == 1) << "dead pixel column at x=" << x;
  }
  // Split at the midpoint: the near half goes to the left card.
  EXPECT_EQ(grid::hitTest(p, a.right(), y), 0);
  EXPECT_EQ(grid::hitTest(p, b.x - 1, y), 1);

  // Same for the horizontal gutter between row 0 and row 1.
  const grid::Rect below = grid::cellRect(p, p.columns);
  const int x = a.x + a.width / 2;
  for (int gy = a.bottom(); gy < below.y; gy++) {
    const int hit = grid::hitTest(p, x, gy);
    EXPECT_TRUE(hit == 0 || hit == p.columns) << "dead pixel row at y=" << gy;
  }
}

TEST(SettingsCategoryGridLayout, NoTwoCardsClaimTheSamePixel) {
  const grid::Plan p = grid::plan(bandFor(480, 800), 6, defaultMetrics());
  ASSERT_TRUE(p.valid());
  const grid::Insets pad = grid::cellHitPadding(p);
  for (int i = 0; i < p.count; i++) {
    for (int j = i + 1; j < p.count; j++) {
      EXPECT_FALSE(overlaps(grid::padded(grid::cellRect(p, i), pad), grid::padded(grid::cellRect(p, j), pad)))
          << "padded hit zones " << i << "," << j << " overlap";
    }
  }
}

TEST(SettingsCategoryGridLayout, TapsOutsideTheGridMiss) {
  const grid::Rect band = bandFor(800, 480);
  const grid::Plan p = grid::plan(band, 6, defaultMetrics());
  ASSERT_TRUE(p.valid());
  EXPECT_EQ(grid::hitTest(p, p.x - p.gap, p.y + 10), -1);                   // left of the grid
  EXPECT_EQ(grid::hitTest(p, p.x + p.gridWidth() + p.gap, p.y + 10), -1);   // right of it
  EXPECT_EQ(grid::hitTest(p, p.x + 10, p.y - p.gap), -1);                   // above it
  EXPECT_EQ(grid::hitTest(p, p.x + 10, p.y + p.gridHeight() + p.gap), -1);  // below it
  EXPECT_EQ(grid::hitTest(p, 0, 0), -1);                                    // the header band
}

TEST(SettingsCategoryGridLayout, PartialLastRowLeavesTheEmptySlotUnclaimed) {
  const grid::Plan p = grid::plan(bandFor(800, 480), 5, defaultMetrics());
  ASSERT_TRUE(p.valid());
  ASSERT_EQ(p.columns, 3);
  // Where a sixth card would have been.
  const grid::Rect fourth = grid::cellRect(p, 4);
  const int x = fourth.x + fourth.width + p.gap + fourth.width / 2;
  EXPECT_EQ(grid::hitTest(p, x, fourth.y + fourth.height / 2), -1);
}

// --- RTL --------------------------------------------------------------------

TEST(SettingsCategoryGridLayout, MirroredColumnIsSelfInverse) {
  for (int columns = 1; columns <= 3; columns++) {
    for (int col = 0; col < columns; col++) {
      EXPECT_EQ(grid::mirroredColumn(grid::mirroredColumn(col, columns, true), columns, true), col);
      EXPECT_EQ(grid::mirroredColumn(col, columns, false), col);
    }
  }
}

TEST(SettingsCategoryGridLayout, RtlMirrorsColumnsButKeepsRowOrder) {
  const grid::Plan p = grid::plan(bandFor(800, 480), 6, defaultMetrics());
  ASSERT_TRUE(p.valid());
  ASSERT_EQ(p.columns, 3);
  // First card sits in the rightmost column, third in the leftmost.
  EXPECT_EQ(grid::cellRect(p, 0, true).x, grid::cellRect(p, 2, false).x);
  EXPECT_EQ(grid::cellRect(p, 2, true).x, grid::cellRect(p, 0, false).x);
  // Rows are unchanged: card 3 still starts the second row.
  EXPECT_EQ(grid::cellRect(p, 3, true).y, grid::cellRect(p, 0, true).y + p.cellHeight + p.gap);
}

TEST(SettingsCategoryGridLayout, RtlHitTestingFollowsRtlRendering) {
  const grid::Plan p = grid::plan(bandFor(800, 480), 6, defaultMetrics());
  ASSERT_TRUE(p.valid());
  for (int i = 0; i < p.count; i++) {
    const grid::Rect c = grid::cellRect(p, i, true);
    EXPECT_EQ(grid::hitTest(p, c.x + c.width / 2, c.y + c.height / 2, true), i);
  }
  // And the two directions really do disagree, so the flag is load-bearing.
  const grid::Rect first = grid::cellRect(p, 0, true);
  EXPECT_EQ(grid::hitTest(p, first.x + first.width / 2, first.y + first.height / 2, false), 2);
}

TEST(SettingsCategoryGridLayout, RtlCellsStillTileTheBandWithoutOverlap) {
  const grid::Plan p = grid::plan(bandFor(480, 800), 6, defaultMetrics());
  ASSERT_TRUE(p.valid());
  std::set<std::string> seen;
  for (int i = 0; i < p.count; i++) {
    const grid::Rect c = grid::cellRect(p, i, true);
    EXPECT_TRUE(seen.insert(std::to_string(c.x) + "," + std::to_string(c.y)).second) << "duplicate origin";
    for (int j = i + 1; j < p.count; j++) {
      EXPECT_FALSE(overlaps(c, grid::cellRect(p, j, true))) << "cells " << i << "," << j;
    }
  }
}
