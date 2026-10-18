// Host tests for the panel-native -> logical touch transform.
//
// This is the gate described in docs/contributing/touch-architecture.md ("Coordinate transform"): the
// orientation transform is pure logic and must be verifiable without hardware.
// Getting a rotation branch wrong is the classic touch bug — taps land mirrored
// or on the wrong axis — and it is exactly the kind of thing that is painful to
// diagnose on device and trivial to catch here.
//
// Panel geometry throughout is the X4 Pro's: 800x480 native (landscape), which
// the reader presents as 480x800 in portrait.

#include <TouchTransform.h>
#include <gtest/gtest.h>

namespace {

constexpr int kPanelW = 800;  // native landscape width
constexpr int kPanelH = 480;  // native landscape height

struct Pt {
  int x;
  int y;
};

Pt map(const int orientation, const float nx, const float ny) {
  Pt p{};
  touchtransform::tapToLogical(orientation, kPanelW, kPanelH, nx, ny, p.x, p.y);
  return p;
}

// LandscapeCounterClockwise is the panel's native frame, so it is the identity
// mapping and the easiest anchor for the other three.
TEST(TouchTransform, LandscapeCcwIsIdentity) {
  const Pt origin = map(touchtransform::LandscapeCounterClockwise, 0.0f, 0.0f);
  EXPECT_EQ(origin.x, 0);
  EXPECT_EQ(origin.y, 0);

  const Pt mid = map(touchtransform::LandscapeCounterClockwise, 0.5f, 0.5f);
  EXPECT_EQ(mid.x, kPanelW / 2);
  EXPECT_EQ(mid.y, kPanelH / 2);

  const Pt far = map(touchtransform::LandscapeCounterClockwise, 1.0f, 1.0f);
  EXPECT_EQ(far.x, kPanelW - 1);
  EXPECT_EQ(far.y, kPanelH - 1);
}

// Portrait renders 480x800: the logical X axis runs along the panel's height
// and is reversed, the logical Y axis runs along the panel's width.
TEST(TouchTransform, PortraitRotatesAndMirrorsX) {
  const Pt origin = map(touchtransform::Portrait, 0.0f, 0.0f);
  EXPECT_EQ(origin.x, kPanelH - 1);
  EXPECT_EQ(origin.y, 0);

  const Pt far = map(touchtransform::Portrait, 1.0f, 1.0f);
  EXPECT_EQ(far.x, 0);
  EXPECT_EQ(far.y, kPanelW - 1);
}

TEST(TouchTransform, PortraitInvertedIsPortraitTurnedAround) {
  const Pt origin = map(touchtransform::PortraitInverted, 0.0f, 0.0f);
  EXPECT_EQ(origin.x, 0);
  EXPECT_EQ(origin.y, kPanelW - 1);

  const Pt far = map(touchtransform::PortraitInverted, 1.0f, 1.0f);
  EXPECT_EQ(far.x, kPanelH - 1);
  EXPECT_EQ(far.y, 0);
}

TEST(TouchTransform, LandscapeClockwiseMirrorsBothAxes) {
  const Pt origin = map(touchtransform::LandscapeClockwise, 0.0f, 0.0f);
  EXPECT_EQ(origin.x, kPanelW - 1);
  EXPECT_EQ(origin.y, kPanelH - 1);

  const Pt far = map(touchtransform::LandscapeClockwise, 1.0f, 1.0f);
  EXPECT_EQ(far.x, 0);
  EXPECT_EQ(far.y, 0);
}

// Every orientation must land inside its own logical screen for any input in
// 0..1 -- the property that actually matters to callers doing hit tests.
TEST(TouchTransform, AllOrientationsStayOnScreen) {
  const int orientations[] = {
      touchtransform::Portrait,
      touchtransform::LandscapeClockwise,
      touchtransform::PortraitInverted,
      touchtransform::LandscapeCounterClockwise,
  };
  for (const int orientation : orientations) {
    // Portrait and PortraitInverted present the panel rotated, so their logical
    // extents are the panel's dimensions swapped.
    const bool portrait = orientation == touchtransform::Portrait || orientation == touchtransform::PortraitInverted;
    const int logicalW = portrait ? kPanelH : kPanelW;
    const int logicalH = portrait ? kPanelW : kPanelH;
    for (float nx = 0.0f; nx <= 1.0f; nx += 0.05f) {
      for (float ny = 0.0f; ny <= 1.0f; ny += 0.05f) {
        const Pt p = map(orientation, nx, ny);
        EXPECT_GE(p.x, 0) << "orientation " << orientation;
        EXPECT_LT(p.x, logicalW) << "orientation " << orientation;
        EXPECT_GE(p.y, 0) << "orientation " << orientation;
        EXPECT_LT(p.y, logicalH) << "orientation " << orientation;
      }
    }
  }
}

// A controller reporting slightly out of range (or exactly 1.0, which would
// otherwise index one pixel past the edge) must be clamped, not wrapped.
TEST(TouchTransform, OutOfRangeInputIsClamped) {
  const Pt low = map(touchtransform::LandscapeCounterClockwise, -0.4f, -1.0f);
  EXPECT_EQ(low.x, 0);
  EXPECT_EQ(low.y, 0);

  const Pt high = map(touchtransform::LandscapeCounterClockwise, 1.7f, 2.5f);
  EXPECT_EQ(high.x, kPanelW - 1);
  EXPECT_EQ(high.y, kPanelH - 1);

  // Clamping happens before rotation, so it holds in a rotated frame too.
  const Pt rotated = map(touchtransform::Portrait, 1.7f, 2.5f);
  EXPECT_EQ(rotated.x, 0);
  EXPECT_EQ(rotated.y, kPanelW - 1);
}

// Opposite orientations must disagree, or a mis-wired transform that ignored
// the orientation argument entirely would still pass everything above.
TEST(TouchTransform, OppositeOrientationsDiffer) {
  const Pt ccw = map(touchtransform::LandscapeCounterClockwise, 0.25f, 0.75f);
  const Pt cw = map(touchtransform::LandscapeClockwise, 0.25f, 0.75f);
  EXPECT_NE(ccw.x, cw.x);
  EXPECT_NE(ccw.y, cw.y);

  const Pt portrait = map(touchtransform::Portrait, 0.25f, 0.75f);
  const Pt inverted = map(touchtransform::PortraitInverted, 0.25f, 0.75f);
  EXPECT_NE(portrait.x, inverted.x);
  EXPECT_NE(portrait.y, inverted.y);
}

// The inverse used to inject touches: every on-screen point survives the round trip in every
// orientation, so a CMD:TOUCH at (x, y) is a tap the screen reads at (x, y).
TEST(TouchTransform, LogicalToTapRoundTripsEveryOrientation) {
  for (const int orientation : {touchtransform::Portrait, touchtransform::LandscapeClockwise,
                                touchtransform::PortraitInverted, touchtransform::LandscapeCounterClockwise}) {
    const bool portrait = orientation == touchtransform::Portrait || orientation == touchtransform::PortraitInverted;
    const int width = portrait ? kPanelH : kPanelW;
    const int height = portrait ? kPanelW : kPanelH;
    for (const Pt in : {Pt{0, 0}, Pt{width - 1, 0}, Pt{0, height - 1}, Pt{width - 1, height - 1}, Pt{123, 456 % height},
                        Pt{width / 2, height / 3}}) {
      float nx = -1.0f;
      float ny = -1.0f;
      touchtransform::logicalToTap(orientation, kPanelW, kPanelH, in.x, in.y, nx, ny);
      EXPECT_GE(nx, 0.0f);
      EXPECT_LT(nx, 1.0f);
      EXPECT_GE(ny, 0.0f);
      EXPECT_LT(ny, 1.0f);
      const Pt out = map(orientation, nx, ny);
      EXPECT_EQ(out.x, in.x) << "orientation " << orientation;
      EXPECT_EQ(out.y, in.y) << "orientation " << orientation;
    }
  }
}

}  // namespace
