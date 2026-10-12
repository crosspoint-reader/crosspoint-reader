#include <gtest/gtest.h>

#include "components/themes/HintLayout.h"

namespace {

// ---- Side (Up/Down) boxes ----

// An X4 in Lyra at the default UI size: 800 px panel, 40 px bottom strip, 5 + 84 px header,
// side boxes 78 px tall with Up's bottom edge at 345 + 78 and a 5 px gap to Down.
constexpr int kPanel = 800;
constexpr int kStrip = 40;
constexpr int kHeader = 5 + 84;
constexpr int kSideBase = 78;
constexpr int kJunction = 345 + kSideBase;
constexpr int kSideGap = 5;

TEST(HintLayoutSide, FreeSpanKeepsClearOfTheHeaderAndTheBottomStrip) {
  const HintLayout::Span portrait = HintLayout::sideFreeSpan(kPanel, kStrip, kHeader, HintLayout::HeaderEdge::PanelTop);
  EXPECT_EQ(kHeader, portrait.top);
  EXPECT_EQ(kPanel - kStrip, portrait.bottom);

  // PortraitInverted puts the strip at the logical top, so the header lies beyond it at the
  // panel bottom.
  const HintLayout::Span inverted =
      HintLayout::sideFreeSpan(kPanel, kStrip, kHeader, HintLayout::HeaderEdge::PanelBottom);
  EXPECT_EQ(0, inverted.top);
  EXPECT_EQ(kPanel - kStrip - kHeader, inverted.bottom);

  const HintLayout::Span landscape = HintLayout::sideFreeSpan(kPanel, kStrip, kHeader, HintLayout::HeaderEdge::None);
  EXPECT_EQ(0, landscape.top);
  EXPECT_EQ(kPanel - kStrip, landscape.bottom);
}

// "Up" and "Down" fit the tuned boxes, so nothing moves: the layout every screen had before
// the boxes could grow.
TEST(HintLayoutSide, ShortLabelsKeepTheTunedBoxes) {
  const HintLayout::Span span = HintLayout::sideFreeSpan(kPanel, kStrip, kHeader, HintLayout::HeaderEdge::PanelTop);
  const auto stack = HintLayout::sideStacked(kJunction, kSideGap, kSideBase, /*needed=*/40, span);
  EXPECT_EQ(345, stack.up.y);
  EXPECT_EQ(kSideBase, stack.up.height);
  EXPECT_EQ(kJunction + kSideGap, stack.down.y);
  EXPECT_EQ(kSideBase, stack.down.height);

  const auto box = HintLayout::sideCentred(155 + kSideBase / 2, kSideBase, /*needed=*/40, span);
  EXPECT_EQ(155, box.y);
  EXPECT_EQ(kSideBase, box.height);
}

// Issue #446: a label longer than the box grows it -- Up upwards, Down downwards -- while the
// edges facing each other stay put beside the buttons.
TEST(HintLayoutSide, LongLabelsGrowAwayFromTheJunction) {
  const HintLayout::Span span = HintLayout::sideFreeSpan(kPanel, kStrip, kHeader, HintLayout::HeaderEdge::PanelTop);
  const auto stack = HintLayout::sideStacked(kJunction, kSideGap, kSideBase, /*needed=*/120, span);
  EXPECT_EQ(120, stack.up.height);
  EXPECT_EQ(kJunction, stack.up.y + stack.up.height);
  EXPECT_EQ(120, stack.down.height);
  EXPECT_EQ(kJunction + kSideGap, stack.down.y);
}

// A label too long for the free stretch stops the boxes at its nearer end; the theme then
// shrinks and clips the label to that length.
TEST(HintLayoutSide, GrowthStopsAtTheNearerEndOfTheFreeStretch) {
  const HintLayout::Span span = HintLayout::sideFreeSpan(kPanel, kStrip, kHeader, HintLayout::HeaderEdge::PanelBottom);
  const auto stack = HintLayout::sideStacked(kJunction, kSideGap, kSideBase, /*needed=*/1000, span);
  // Inverted, the header shortens the panel bottom: Down has less room than Up, and both
  // boxes take Down's length so the pair stays even.
  const int downRoom = span.bottom - (kJunction + kSideGap);
  ASSERT_LT(downRoom, kJunction - span.top);
  EXPECT_EQ(downRoom, stack.down.height);
  EXPECT_EQ(span.bottom, stack.down.y + stack.down.height);
  EXPECT_EQ(downRoom, stack.up.height);
  EXPECT_GE(stack.up.y, span.top);
}

// X3: one box per side edge, level with its button, so it grows equally both ways and is
// bounded by whichever end of the stretch is nearer -- the header, at 155 px down.
TEST(HintLayoutSide, CentredBoxGrowsBothWaysWithinTheStretch) {
  const HintLayout::Span span = HintLayout::sideFreeSpan(792, kStrip, kHeader, HintLayout::HeaderEdge::PanelTop);
  const int centre = 155 + kSideBase / 2;

  const auto grown = HintLayout::sideCentred(centre, kSideBase, /*needed=*/120, span);
  EXPECT_EQ(120, grown.height);
  EXPECT_EQ(centre - 60, grown.y);

  const auto capped = HintLayout::sideCentred(centre, kSideBase, /*needed=*/1000, span);
  EXPECT_EQ(span.top, capped.y);
  EXPECT_EQ(2 * (centre - span.top), capped.height);
}

// The one sizing rule both strips share: as far as the label needs, never below the tuned box,
// never past the room -- and when even the tuned box does not fit the room, the tuned box wins.
TEST(HintLayout, GrowStaysWithinTheTunedSizeAndTheRoom) {
  EXPECT_EQ(kSideBase, HintLayout::grow(kSideBase, /*needed=*/200, /*room=*/50));
  EXPECT_EQ(kSideBase, HintLayout::grow(kSideBase, /*needed=*/10, /*room=*/500));
  EXPECT_EQ(150, HintLayout::grow(kSideBase, /*needed=*/150, /*room=*/500));
}

// ---- Bottom strip ----

// The hand-tuned layouts drawButtonHints() starts from.
constexpr int kLyraX4[HintLayout::BOTTOM_COUNT] = {58, 146, 254, 342};    // 80 px boxes on a 480 px panel
constexpr int kLyraX3[HintLayout::BOTTOM_COUNT] = {65, 157, 291, 383};    // 80 px boxes on a 528 px panel
constexpr int kBaseX4[HintLayout::BOTTOM_COUNT] = {25, 130, 245, 350};    // 106 px boxes; 0/1 and 2/3 overlap by 1 px
constexpr int kEvenT5S3[HintLayout::BOTTOM_COUNT] = {44, 168, 292, 416};  // ButtonHintLayout's even spread, 540 px

void expectTunedLayoutKept(const int screenWidth, const int base, const int (&tuned)[HintLayout::BOTTOM_COUNT]) {
  const int widths[HintLayout::BOTTOM_COUNT] = {base, base, base, base};
  int out[HintLayout::BOTTOM_COUNT];
  HintLayout::placeBottom(screenWidth, base, tuned, widths, out);
  for (int i = 0; i < HintLayout::BOTTOM_COUNT; ++i) EXPECT_EQ(tuned[i], out[i]) << "box " << i;
}

// Labels that fit the tuned boxes leave every box exactly where it was measured to sit,
// including the Classic layout's 1 px overlaps.
TEST(HintLayoutBottom, FittingLabelsKeepTheTunedLayout) {
  expectTunedLayoutKept(480, 80, kLyraX4);
  expectTunedLayoutKept(528, 80, kLyraX3);
  expectTunedLayoutKept(480, 106, kBaseX4);
  expectTunedLayoutKept(540, 80, kEvenT5S3);
}

// A fair quarter: what is left once the gaps and margins are out, split four ways.
TEST(HintLayoutBottom, MaxWidthIsAFairQuarterOfThePanel) {
  EXPECT_EQ((480 - 2 * 4 - 3 * 4) / 4, HintLayout::bottomMaxWidth(480, 80, kLyraX4));
  // Classic's tuned 1 px overlaps stand in for two of the gaps.
  EXPECT_EQ((480 - 2 * 4 - (-1 + 4 - 1)) / 4, HintLayout::bottomMaxWidth(480, 106, kBaseX4));
  EXPECT_LE(HintLayout::bottomMaxWidth(528, 80, kLyraX3), 528 / 4);
}

// "Вправо / Вкладка" in Lyra's second box (issue #446). The grown box takes the room it needs,
// its neighbour on the crowded side moves over, and the far pair does not move at all.
TEST(HintLayoutBottom, AGrownBoxMovesItsNeighboursOnlyAsFarAsNeeded) {
  const int max = HintLayout::bottomMaxWidth(480, 80, kLyraX4);
  const int widths[HintLayout::BOTTOM_COUNT] = {80, max, 80, 80};
  int out[HintLayout::BOTTOM_COUNT];
  HintLayout::placeBottom(480, 80, kLyraX4, widths, out);

  EXPECT_LT(out[0], kLyraX4[0]);
  EXPECT_GE(out[1], out[0] + widths[0] + HintLayout::BOTTOM_MIN_GAP);
  EXPECT_GE(out[2], out[1] + widths[1] + HintLayout::BOTTOM_MIN_GAP);
  EXPECT_EQ(kLyraX4[2], out[2]);
  EXPECT_EQ(kLyraX4[3], out[3]);
  // Still roughly above its own button.
  EXPECT_NEAR(kLyraX4[1] + 40, out[1] + widths[1] / 2, 10);
}

// Every box at the cap still fits on the panel without touching another.
TEST(HintLayoutBottom, AllFourAtTheCapStillFit) {
  for (const int panel : {480, 528}) {
    const auto& tuned = panel == 480 ? kLyraX4 : kLyraX3;
    const int max = HintLayout::bottomMaxWidth(panel, 80, tuned);
    const int widths[HintLayout::BOTTOM_COUNT] = {max, max, max, max};
    int out[HintLayout::BOTTOM_COUNT];
    HintLayout::placeBottom(panel, 80, tuned, widths, out);
    EXPECT_GE(out[0], HintLayout::BOTTOM_EDGE_MARGIN) << panel;
    EXPECT_LE(out[3] + max, panel - HintLayout::BOTTOM_EDGE_MARGIN) << panel;
    for (int i = 0; i + 1 < HintLayout::BOTTOM_COUNT; ++i) {
      EXPECT_GE(out[i + 1], out[i] + max + HintLayout::BOTTOM_MIN_GAP) << panel << " box " << i;
    }
  }
}

// An outer box grows towards the panel edge it has room at rather than into its pair.
TEST(HintLayoutBottom, AnOuterBoxStaysOnThePanel) {
  const int max = HintLayout::bottomMaxWidth(480, 80, kLyraX4);
  const int widths[HintLayout::BOTTOM_COUNT] = {80, 80, 80, max};
  int out[HintLayout::BOTTOM_COUNT];
  HintLayout::placeBottom(480, 80, kLyraX4, widths, out);
  EXPECT_LE(out[3] + max, 480 - HintLayout::BOTTOM_EDGE_MARGIN);
  EXPECT_GE(out[3], out[2] + 80 + HintLayout::BOTTOM_MIN_GAP);
  EXPECT_EQ(kLyraX4[0], out[0]);
}

}  // namespace
