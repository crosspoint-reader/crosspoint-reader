#include <gtest/gtest.h>

#include "TouchTransform.h"
#include "components/themes/ButtonHintStrip.h"

namespace {

using ButtonHintStrip::Strip;

// A strip shaped like the one BaseTheme paints on an X4: four 106px boxes at the tuned
// positions, in a 42px band at the bottom of an 800px-tall portrait screen.
Strip x4Strip() {
  Strip s;
  s.y = 800 - 42;
  s.height = 42;
  for (int& w : s.width) w = 106;
  s.x[0] = 25;
  s.x[1] = 130;
  s.x[2] = 245;
  s.x[3] = 350;
  for (bool& a : s.active) a = true;
  return s;
}

TEST(ButtonHintStrip, HitsEachBoxAtItsCentre) {
  const Strip s = x4Strip();
  const int y = s.y + s.height / 2;
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(i, ButtonHintStrip::hitTestIn(s, s.x[i] + s.width[i] / 2, y)) << "box " << i;
  }
}

// The index IS the raw hardware button (BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT), which
// is what lets the caller skip any mapping. Pin the order so a reshuffle of mapLabels()
// cannot silently start firing the wrong button.
TEST(ButtonHintStrip, LeftmostBoxIsRawButtonZero) {
  const Strip s = x4Strip();
  EXPECT_EQ(0, ButtonHintStrip::hitTestIn(s, s.x[0], s.y));
  EXPECT_EQ(3, ButtonHintStrip::hitTestIn(s, s.x[3] + s.width[3] - 1, s.y + s.height - 1));
}

TEST(ButtonHintStrip, BoundariesAreHalfOpen) {
  const Strip s = x4Strip();
  const int y = s.y + 1;
  // First px of a box hits; the px one past its right edge does not. Uses box 2, the one
  // pair with real space either side of it (see TunedBoxesOverlapByOnePixel).
  EXPECT_EQ(2, ButtonHintStrip::hitTestIn(s, s.x[2], y));
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, s.x[2] - 1, y));
  EXPECT_EQ(3, ButtonHintStrip::hitTestIn(s, s.x[2] + s.width[2], y));  // = x[3], box 3 starts here
  // Same on the vertical axis: the row above the band and the row past it both miss.
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, s.x[2], s.y - 1));
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, s.x[2], s.y + s.height));
}

// The hand-tuned X4 positions {25,130,245,350} at width 106 are not disjoint: box 0 spans
// 25..130 and box 1 starts at 130, so one column belongs to both. hitTestIn scans in order
// and awards it to the lower index. Immaterial to a finger, but pinned here so the tie-break
// is a decision on record rather than an accident of loop order -- and so that anyone
// re-tuning the positions sees that these boxes were never disjoint to begin with.
TEST(ButtonHintStrip, TunedBoxesOverlapByOnePixel) {
  const Strip s = x4Strip();
  const int y = s.y + s.height / 2;
  ASSERT_EQ(s.x[0] + s.width[0], s.x[1] + 1) << "x4 box 0/1 overlap assumption no longer holds";
  EXPECT_EQ(0, ButtonHintStrip::hitTestIn(s, s.x[1], y));
  EXPECT_EQ(1, ButtonHintStrip::hitTestIn(s, s.x[1] + 1, y));
}

TEST(ButtonHintStrip, MissesOutsideTheBoxes) {
  const Strip s = x4Strip();
  const int y = s.y + s.height / 2;
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, 0, y));                        // left of box 0
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, s.x[1] + s.width[1] + 2, y));  // real gap, 1 -> 2
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, s.x[3] + s.width[3] + 1, y));  // right of box 3
}

// A box that grew for a long label (HintLayout) is tappable across its own width, and
// its narrower neighbours only across theirs.
TEST(ButtonHintStrip, EachBoxHasItsOwnWidth) {
  Strip s = x4Strip();
  s.x[2] = 240;
  s.width[2] = 116;
  s.x[3] = 360;
  const int y = s.y + s.height / 2;
  EXPECT_EQ(2, ButtonHintStrip::hitTestIn(s, s.x[2] + 112, y));
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, s.x[1] + s.width[1] + 2, y));
}

// An empty label paints no full-size box (BaseTheme) or a decorative stub (LyraTheme).
// Either way there is no action behind it, so the band must stay dead there.
TEST(ButtonHintStrip, InactiveBoxIsNotTappable) {
  Strip s = x4Strip();
  s.active[2] = false;
  const int y = s.y + s.height / 2;
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, s.x[2] + s.width[2] / 2, y));
  // Its neighbours still work — an inactive box must not blank the whole strip.
  EXPECT_EQ(1, ButtonHintStrip::hitTestIn(s, s.x[1] + s.width[1] / 2, y));
  EXPECT_EQ(3, ButtonHintStrip::hitTestIn(s, s.x[3] + s.width[3] / 2, y));
}

// invalidate() leaves a default Strip. Every tap must miss it, which is what stops a screen
// that draws no hints from inheriting the previous screen's boxes.
TEST(ButtonHintStrip, DefaultStripSwallowsNothing) {
  const Strip s;
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, 0, 0));
  EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, 240, 780));
}

// The shared-state path: record/hasStrip/hitTest against the seqlock.
TEST(ButtonHintStrip, RecordThenHitTestThroughSharedState) {
  ButtonHintStrip::invalidate();
  EXPECT_FALSE(ButtonHintStrip::hasStrip());
  EXPECT_EQ(-1, ButtonHintStrip::hitTest(25, 780));

  const Strip s = x4Strip();
  ButtonHintStrip::record(s);
  EXPECT_TRUE(ButtonHintStrip::hasStrip());
  EXPECT_EQ(0, ButtonHintStrip::hitTest(s.x[0] + 1, s.y + 1));
  EXPECT_EQ(2, ButtonHintStrip::hitTest(s.x[2] + 1, s.y + 1));

  ButtonHintStrip::invalidate();
  EXPECT_FALSE(ButtonHintStrip::hasStrip());
  EXPECT_EQ(-1, ButtonHintStrip::hitTest(s.x[0] + 1, s.y + 1));
}

// The strip is recorded in the Portrait frame however the screen is rotated, because
// drawButtonHints() forces Portrait for its draw. So ActivityManager::dispatchHintStripTap()
// resolves the tap with an explicit touchtransform::Portrait rather than the live orientation,
// which is what keeps the strip tappable in all four orientations. That dispatcher needs the
// HAL and the renderer, so it cannot run here. What this pins is the geometry the explicit
// Portrait rests on: a device-logged contact resolves onto its box in the Portrait frame, and
// resolving the same contact against any rotated live frame misses the strip entirely -- the
// bug a live-orientation hit test would have.
TEST(ButtonHintStrip, TapResolvesOntoTheStripOnlyInThePortraitFrame) {
  // T5S3: 960x540 native panel, 540x960 portrait logical frame, Lyra even-spread boxes.
  constexpr int panelWidth = 960;
  constexpr int panelHeight = 540;
  Strip s;
  s.y = 960 - 30;
  s.height = 30;
  for (int& w : s.width) w = 80;
  s.x[0] = 44;
  s.x[1] = 168;
  s.x[2] = 292;
  s.x[3] = 416;
  for (bool& a : s.active) a = true;

  // Taken from a device log: this contact resolved to portrait (481,937) = box 3.
  constexpr float nx = 0.977f;
  constexpr float ny = 0.108f;

  int x = 0;
  int y = 0;
  touchtransform::tapToLogical(touchtransform::Portrait, panelWidth, panelHeight, nx, ny, x, y);
  EXPECT_EQ(481, x);
  EXPECT_EQ(937, y);
  EXPECT_EQ(3, ButtonHintStrip::hitTestIn(s, x, y));

  const int rotated[] = {
      touchtransform::LandscapeClockwise,
      touchtransform::PortraitInverted,
      touchtransform::LandscapeCounterClockwise,
  };
  for (const int live : rotated) {
    int lx = 0;
    int ly = 0;
    touchtransform::tapToLogical(live, panelWidth, panelHeight, nx, ny, lx, ly);
    EXPECT_EQ(-1, ButtonHintStrip::hitTestIn(s, lx, ly))
        << "orientation " << live << " resolved to (" << lx << "," << ly << ")";
  }
}

// A strip whose labels are all empty is not a strip: skipping the tap queue entirely on
// such a screen leaves the tap for whoever else wants it.
TEST(ButtonHintStrip, AllInactiveCountsAsNoStrip) {
  Strip s = x4Strip();
  for (bool& a : s.active) a = false;
  ButtonHintStrip::record(s);
  EXPECT_FALSE(ButtonHintStrip::hasStrip());
  ButtonHintStrip::invalidate();
}

using ButtonHintStrip::SideStrip;

// Shaped like the X4 layout in LyraTheme::drawSideButtonHints: two 80x78 boxes stacked in one
// column on the right edge, slot 0 (BTN_UP) above slot 1 (BTN_DOWN) with a 5px gap.
SideStrip x4SideStrip() {
  SideStrip s;
  s.x[0] = 460;
  s.y[0] = 345;
  s.width[0] = 80;
  s.height[0] = 78;
  s.active[0] = true;
  s.x[1] = 460;
  s.y[1] = 345 + 78 + 5;
  s.width[1] = 80;
  s.height[1] = 78;
  s.active[1] = true;
  return s;
}

TEST(ButtonHintStripSide, HitsEachBoxAtItsCentre) {
  const SideStrip s = x4SideStrip();
  for (int i = 0; i < 2; ++i) {
    const int x = s.x[i] + s.width[i] / 2;
    const int y = s.y[i] + s.height[i] / 2;
    EXPECT_EQ(i, ButtonHintStrip::Side::hitTestIn(s, x, y)) << "box " << i;
  }
}

TEST(ButtonHintStripSide, BoundariesAreHalfOpen) {
  const SideStrip s = x4SideStrip();
  // First px of box 1 hits; the px one above its top edge (still inside the gap) does not.
  EXPECT_EQ(1, ButtonHintStrip::Side::hitTestIn(s, s.x[1], s.y[1]));
  EXPECT_EQ(-1, ButtonHintStrip::Side::hitTestIn(s, s.x[1], s.y[1] - 1));
  EXPECT_EQ(-1, ButtonHintStrip::Side::hitTestIn(s, s.x[1], s.y[1] + s.height[1]));
}

// The gap between the two stacked boxes (X4's 5px spacer) must stay dead, unlike the bottom
// strip whose tuned positions are deliberately allowed to overlap.
TEST(ButtonHintStripSide, GapBetweenBoxesMisses) {
  const SideStrip s = x4SideStrip();
  const int gapY = s.y[0] + s.height[0] + 2;  // inside the 5px gap
  EXPECT_EQ(-1, ButtonHintStrip::Side::hitTestIn(s, s.x[0] + s.width[0] / 2, gapY));
}

// An inactive box (one side hint with no label, e.g. Left/Right bound to Up/Down instead in
// landscape) must not swallow taps meant for its still-active neighbour.
TEST(ButtonHintStripSide, InactiveBoxIsNotTappable) {
  SideStrip s = x4SideStrip();
  s.active[0] = false;
  EXPECT_EQ(-1, ButtonHintStrip::Side::hitTestIn(s, s.x[0] + s.width[0] / 2, s.y[0] + s.height[0] / 2));
  EXPECT_EQ(1, ButtonHintStrip::Side::hitTestIn(s, s.x[1] + s.width[1] / 2, s.y[1] + s.height[1] / 2));
}

TEST(ButtonHintStripSide, DefaultStripSwallowsNothing) {
  const SideStrip s;
  EXPECT_EQ(-1, ButtonHintStrip::Side::hitTestIn(s, 0, 0));
  EXPECT_EQ(-1, ButtonHintStrip::Side::hitTestIn(s, 460, 400));
}

// The shared-state path: record/hasStrip/hitTest through the Side seqlock, independent of the
// bottom strip's.
TEST(ButtonHintStripSide, RecordThenHitTestThroughSharedState) {
  ButtonHintStrip::Side::invalidate();
  EXPECT_FALSE(ButtonHintStrip::Side::hasStrip());
  EXPECT_EQ(-1, ButtonHintStrip::Side::hitTest(460, 350));

  const SideStrip s = x4SideStrip();
  ButtonHintStrip::Side::record(s);
  EXPECT_TRUE(ButtonHintStrip::Side::hasStrip());
  EXPECT_EQ(0, ButtonHintStrip::Side::hitTest(s.x[0] + 1, s.y[0] + 1));
  EXPECT_EQ(1, ButtonHintStrip::Side::hitTest(s.x[1] + 1, s.y[1] + 1));

  ButtonHintStrip::Side::invalidate();
  EXPECT_FALSE(ButtonHintStrip::Side::hasStrip());
  EXPECT_EQ(-1, ButtonHintStrip::Side::hitTest(s.x[0] + 1, s.y[0] + 1));
}

// Recording one strip must not disturb the other: a reader screen paints both the bottom strip
// and the side hints every frame, and each is published through its own seqlock precisely so
// one's write can never tear the other's read.
TEST(ButtonHintStripSide, IndependentOfBottomStrip) {
  ButtonHintStrip::invalidate();
  ButtonHintStrip::Side::invalidate();

  ButtonHintStrip::record(x4Strip());
  ButtonHintStrip::Side::record(x4SideStrip());
  EXPECT_TRUE(ButtonHintStrip::hasStrip());
  EXPECT_TRUE(ButtonHintStrip::Side::hasStrip());

  ButtonHintStrip::invalidate();
  EXPECT_FALSE(ButtonHintStrip::hasStrip());
  EXPECT_TRUE(ButtonHintStrip::Side::hasStrip());  // untouched by the bottom strip's invalidate

  ButtonHintStrip::Side::invalidate();
}

TEST(ButtonHintStripSide, AllInactiveCountsAsNoStrip) {
  SideStrip s = x4SideStrip();
  s.active[0] = false;
  s.active[1] = false;
  ButtonHintStrip::Side::record(s);
  EXPECT_FALSE(ButtonHintStrip::Side::hasStrip());
  ButtonHintStrip::Side::invalidate();
}

}  // namespace
