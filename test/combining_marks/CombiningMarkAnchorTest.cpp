#include <gtest/gtest.h>

#include "lib/EpdFont/EpdFontData.h"

// ============================================================================
// Anchor selection and placement for combining marks without GPOS tables.
//
// Hebrew niqqud whose identity depends on position must not use the default
// centre-and-raise heuristic: dagesh sits inside the letter body, the
// shin/sin dots sit over the letter's right/left arm, and holam hangs over
// the left corner (see PR #2541 review feedback).
// ============================================================================

using combiningMark::Anchor;
using combiningMark::anchorFor;
using combiningMark::anchorOver;
using combiningMark::anchorOverRotated90CW;
using combiningMark::lowerBelowBase;
using combiningMark::raiseAboveBase;

TEST(AnchorFor, PositionSensitiveNiqqud) {
  EXPECT_EQ(anchorFor(0x05BC), Anchor::CenterNative);  // dagesh/mapiq
  EXPECT_EQ(anchorFor(0x05BA), Anchor::CenterNative);  // holam haser for vav
  EXPECT_EQ(anchorFor(0x05C1), Anchor::RightNative);   // shin dot
  EXPECT_EQ(anchorFor(0x05C2), Anchor::LeftNative);    // sin dot
  EXPECT_EQ(anchorFor(0x05B9), Anchor::LeftNative);    // holam
}

TEST(AnchorFor, EverythingElseKeepsCentreRaisedDefault) {
  EXPECT_EQ(anchorFor(0x05B0), Anchor::CenterRaised);  // Hebrew sheva
  EXPECT_EQ(anchorFor(0x05B7), Anchor::CenterRaised);  // Hebrew patach
  EXPECT_EQ(anchorFor(0x064E), Anchor::CenterRaised);  // Arabic fatha
  EXPECT_EQ(anchorFor(0x0651), Anchor::CenterRaised);  // Arabic shadda
  EXPECT_EQ(anchorFor(0x0301), Anchor::CenterRaised);  // combining acute
}

// Base glyph: cursor 100, left 1, width 12.  Mark: left 2, width 4.
TEST(AnchorOver, HorizontalPlacementPerAnchor) {
  // Centered: base bitmap spans [101, 113), centre 107; mark starts at 105.
  EXPECT_EQ(anchorOver(Anchor::CenterRaised, 100, 1, 12, 2, 4), 105 - 2);
  EXPECT_EQ(anchorOver(Anchor::CenterNative, 100, 1, 12, 2, 4), 105 - 2);
  // Left-aligned: mark bitmap starts at the base bitmap's left edge (101).
  EXPECT_EQ(anchorOver(Anchor::LeftNative, 100, 1, 12, 2, 4), 101 - 2);
  // Right-aligned: mark bitmap ends at the base bitmap's right edge (113).
  EXPECT_EQ(anchorOver(Anchor::RightNative, 100, 1, 12, 2, 4), 109 - 2);
}

// The rotated coordinate system inverts every left/width term, so the mark's
// offset from the base cursor must be the exact mirror of the unrotated one.
TEST(AnchorOverRotated90CW, MirrorsUnrotatedOffsets) {
  for (const Anchor anchor : {Anchor::CenterRaised, Anchor::CenterNative, Anchor::LeftNative, Anchor::RightNative}) {
    const int offset = anchorOver(anchor, 100, 1, 12, 2, 4) - 100;
    EXPECT_EQ(anchorOverRotated90CW(anchor, 100, 1, 12, 2, 4), 100 - offset);
  }
}

TEST(RaiseAboveBase, NativeAnchorsKeepFontDesignedHeight) {
  // A dagesh-like dot designed inside the letter body (top 8 of a base whose
  // top is 12) must NOT be hoisted above the letter.
  EXPECT_EQ(raiseAboveBase(Anchor::CenterNative, 8, 3, 12), 0);
  // Shin/sin dots overlapping the letter's top must not be pushed clear.
  EXPECT_EQ(raiseAboveBase(Anchor::RightNative, 13, 3, 12), 0);
  EXPECT_EQ(raiseAboveBase(Anchor::LeftNative, 13, 3, 12), 0);
}

TEST(RaiseAboveBase, CentreRaisedBehaviourUnchanged) {
  // Above-baseline mark colliding with the base: raised to restore a 1px gap.
  // gap = markTop - markHeight - baseTop = 10 - 3 - 12 = -5  ->  raise 6.
  EXPECT_EQ(raiseAboveBase(Anchor::CenterRaised, 10, 3, 12), 6);
  // Already clear of the base: no raise.
  EXPECT_EQ(raiseAboveBase(Anchor::CenterRaised, 16, 3, 12), 0);
  // Below-baseline mark (kasra, cedilla): stays at font-native position.
  EXPECT_EQ(raiseAboveBase(Anchor::CenterRaised, 2, 4, 12), 0);
}

// A below-baseline mark (top -3, height 6) lowered clear of a base glyph that
// descends past it. Font descender -18. Values from a real 16px face.
TEST(LowerBelowBase, ClearsDescendingBase) {
  // Base bottom -8, no clamp: lower = -3 - (-8 - 1) = 6.
  EXPECT_EQ(lowerBelowBase(Anchor::CenterRaised, -3, 6, 12, 20, -18), 6);
}

TEST(LowerBelowBase, ClampsToLineDescender) {
  // Deep base (bottom -12): ideal lower 10 puts the mark at -19, clamped to 9.
  EXPECT_EQ(lowerBelowBase(Anchor::CenterRaised, -3, 6, 22, 34, -18), 9);
}

TEST(LowerBelowBase, NoLowerWhenBaseDoesNotDescendIntoMark) {
  // Base sitting on the baseline: mark already clear.
  EXPECT_EQ(lowerBelowBase(Anchor::CenterRaised, -3, 6, 20, 20, -18), 0);
}

TEST(LowerBelowBase, NoLowerWithoutBaseGlyph) {
  // No base glyph (baseHeight 0): a leading mark, or a mark after a zero-height
  // space. Nothing to clear, so the mark must not be pushed against a phantom
  // baseline. E.g. a leading U+0318 (combining tack below).
  EXPECT_EQ(lowerBelowBase(Anchor::CenterRaised, 0, 3, 0, 0, -18), 0);
  EXPECT_EQ(lowerBelowBase(Anchor::CenterRaised, -3, 6, 0, 0, -18), 0);
}

TEST(LowerBelowBase, IgnoresAboveBaselineMarksAndNativeAnchors) {
  EXPECT_EQ(lowerBelowBase(Anchor::CenterRaised, 23, 5, 22, 34, -18), 0);  // above-baseline: raiseAboveBase owns it
  EXPECT_EQ(lowerBelowBase(Anchor::CenterNative, -3, 6, 22, 34, -18), 0);  // native anchors keep font height
  EXPECT_EQ(lowerBelowBase(Anchor::RightNative, -3, 6, 22, 34, -18), 0);
}
