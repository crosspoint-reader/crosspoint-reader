#include <gtest/gtest.h>

#include "components/themes/SideHintLayout.h"

namespace {

using SideHintLayout::HeaderEdge;
using SideHintLayout::Span;

// An X4 in Lyra at the default UI size: 800 px panel, 40 px bottom strip, 5 + 84 px header,
// side boxes 78 px tall with Up's bottom edge at 345 + 78 and a 5 px gap to Down.
constexpr int kPanel = 800;
constexpr int kStrip = 40;
constexpr int kHeader = 5 + 84;
constexpr int kBase = 78;
constexpr int kJunction = 345 + kBase;
constexpr int kGap = 5;

TEST(SideHintLayout, FreeSpanKeepsClearOfTheHeaderAndTheBottomStrip) {
  const Span portrait = SideHintLayout::freeSpan(kPanel, kStrip, kHeader, HeaderEdge::PanelTop);
  EXPECT_EQ(kHeader, portrait.top);
  EXPECT_EQ(kPanel - kStrip, portrait.bottom);

  // PortraitInverted puts the strip at the logical top, so the header lies beyond it at the
  // panel bottom.
  const Span inverted = SideHintLayout::freeSpan(kPanel, kStrip, kHeader, HeaderEdge::PanelBottom);
  EXPECT_EQ(0, inverted.top);
  EXPECT_EQ(kPanel - kStrip - kHeader, inverted.bottom);

  const Span landscape = SideHintLayout::freeSpan(kPanel, kStrip, kHeader, HeaderEdge::None);
  EXPECT_EQ(0, landscape.top);
  EXPECT_EQ(kPanel - kStrip, landscape.bottom);
}

// "Up" and "Down" fit the tuned boxes, so nothing moves: the layout every screen had before
// the boxes could grow.
TEST(SideHintLayout, ShortLabelsKeepTheTunedBoxes) {
  const Span span = SideHintLayout::freeSpan(kPanel, kStrip, kHeader, HeaderEdge::PanelTop);
  const auto stack = SideHintLayout::stacked(kJunction, kGap, kBase, /*needed=*/40, span);
  EXPECT_EQ(345, stack.up.y);
  EXPECT_EQ(kBase, stack.up.height);
  EXPECT_EQ(kJunction + kGap, stack.down.y);
  EXPECT_EQ(kBase, stack.down.height);

  const auto box = SideHintLayout::centred(155 + kBase / 2, kBase, /*needed=*/40, span);
  EXPECT_EQ(155, box.y);
  EXPECT_EQ(kBase, box.height);
}

// Issue #446: a label longer than the box grows it -- Up upwards, Down downwards -- while the
// edges facing each other stay put beside the buttons.
TEST(SideHintLayout, LongLabelsGrowAwayFromTheJunction) {
  const Span span = SideHintLayout::freeSpan(kPanel, kStrip, kHeader, HeaderEdge::PanelTop);
  const auto stack = SideHintLayout::stacked(kJunction, kGap, kBase, /*needed=*/120, span);
  EXPECT_EQ(120, stack.up.height);
  EXPECT_EQ(kJunction, stack.up.y + stack.up.height);
  EXPECT_EQ(120, stack.down.height);
  EXPECT_EQ(kJunction + kGap, stack.down.y);
}

// A label too long for the free stretch stops the boxes at its nearer end; the theme then
// shrinks and clips the label to that length.
TEST(SideHintLayout, GrowthStopsAtTheNearerEndOfTheFreeStretch) {
  const Span span = SideHintLayout::freeSpan(kPanel, kStrip, kHeader, HeaderEdge::PanelBottom);
  const auto stack = SideHintLayout::stacked(kJunction, kGap, kBase, /*needed=*/1000, span);
  // Inverted, the header shortens the panel bottom: Down has less room than Up, and both
  // boxes take Down's length so the pair stays even.
  const int downRoom = span.bottom - (kJunction + kGap);
  ASSERT_LT(downRoom, kJunction - span.top);
  EXPECT_EQ(downRoom, stack.down.height);
  EXPECT_EQ(span.bottom, stack.down.y + stack.down.height);
  EXPECT_EQ(downRoom, stack.up.height);
  EXPECT_GE(stack.up.y, span.top);
}

// X3: one box per side edge, level with its button, so it grows equally both ways and is
// bounded by whichever end of the stretch is nearer -- the header, at 155 px down.
TEST(SideHintLayout, CentredBoxGrowsBothWaysWithinTheStretch) {
  const Span span = SideHintLayout::freeSpan(792, kStrip, kHeader, HeaderEdge::PanelTop);
  const int centre = 155 + kBase / 2;

  const auto grown = SideHintLayout::centred(centre, kBase, /*needed=*/120, span);
  EXPECT_EQ(120, grown.height);
  EXPECT_EQ(centre - 60, grown.y);

  const auto capped = SideHintLayout::centred(centre, kBase, /*needed=*/1000, span);
  EXPECT_EQ(span.top, capped.y);
  EXPECT_EQ(2 * (centre - span.top), capped.height);
}

// A stretch shorter than the tuned box (a panel or metrics nobody tuned for) never shrinks
// the box below what it always was.
TEST(SideHintLayout, TunedLengthWinsOverTooLittleRoom) {
  EXPECT_EQ(kBase, SideHintLayout::boxLength(kBase, /*needed=*/200, /*room=*/50));
  EXPECT_EQ(kBase, SideHintLayout::boxLength(kBase, /*needed=*/10, /*room=*/500));
  EXPECT_EQ(150, SideHintLayout::boxLength(kBase, /*needed=*/150, /*room=*/500));
}

}  // namespace
