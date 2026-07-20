#include <gtest/gtest.h>

#include <array>

#include "components/themes/dashboard/DashboardLayout.h"

namespace {

bool contains(const Rect& outer, const Rect& inner) {
  return inner.x >= outer.x && inner.y >= outer.y && inner.width >= 0 && inner.height >= 0 &&
         inner.x + inner.width <= outer.x + outer.width && inner.y + inner.height <= outer.y + outer.height;
}

bool overlaps(const Rect& a, const Rect& b) {
  return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0 && a.x < b.x + b.width && a.x + a.width > b.x &&
         a.y < b.y + b.height && a.y + a.height > b.y;
}

}  // namespace

TEST(DashboardLayout, FitsBothSupportedPortraitPanels) {
  for (const Rect tile : std::array<Rect, 2>{Rect{0, 56, 480, 410}, Rect{0, 56, 528, 410}}) {
    const DashboardLayout layout = DashboardLayout::calculate(tile);

    EXPECT_TRUE(contains(tile, layout.content));
    EXPECT_TRUE(contains(tile, layout.cover));
    EXPECT_TRUE(contains(tile, layout.stats));
    EXPECT_TRUE(contains(tile, layout.title));
    EXPECT_TRUE(contains(tile, layout.progress));
    EXPECT_TRUE(contains(tile, layout.footer));

    EXPECT_FALSE(overlaps(layout.cover, layout.stats));
    EXPECT_FALSE(overlaps(layout.cover, layout.title));
    EXPECT_FALSE(overlaps(layout.stats, layout.title));
    EXPECT_FALSE(overlaps(layout.title, layout.progress));
    EXPECT_FALSE(overlaps(layout.progress, layout.footer));
  }
}

TEST(DashboardLayout, UsesOnlyTheCoverForTheReusableBitmapCache) {
  const DashboardLayout x4 = DashboardLayout::calculate(Rect{0, 56, 480, 410});
  const DashboardLayout x3 = DashboardLayout::calculate(Rect{0, 56, 528, 410});

  EXPECT_EQ(x4.cover.width, 168);
  EXPECT_EQ(x4.cover.height, 252);
  EXPECT_EQ(x3.cover.width, 168);
  EXPECT_EQ(x3.cover.height, 252);

  // One bit per pixel, rounded per scanline by the renderer. This upper bound
  // stays well below CrossInk's 41-46 KB whole-dashboard snapshot.
  EXPECT_LT(((x4.cover.width + 7) / 8) * x4.cover.height, 6000);
  EXPECT_LT(((x3.cover.width + 7) / 8) * x3.cover.height, 6000);
}

TEST(DashboardLayout, DegradesWithoutNegativeOrOutOfBoundsRects) {
  const Rect tinyTile{10, 20, 120, 100};
  const DashboardLayout layout = DashboardLayout::calculate(tinyTile);

  EXPECT_TRUE(contains(tinyTile, layout.content));
  EXPECT_TRUE(contains(tinyTile, layout.cover));
  EXPECT_TRUE(contains(tinyTile, layout.stats));
  EXPECT_TRUE(contains(tinyTile, layout.title));
  EXPECT_TRUE(contains(tinyTile, layout.progress));
  EXPECT_TRUE(contains(tinyTile, layout.footer));
}
