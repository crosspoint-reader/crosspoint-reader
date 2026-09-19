#include <Epub/blocks/BlockStyle.h>
#include <gtest/gtest.h>

namespace {

constexpr uint16_t VIEWPORT = 480;
constexpr int16_t CONTENT = 200;

// Mirrors fromCssStyle for a block whose CSS declares text-align.
BlockStyle declared(const CssTextAlign alignment, const int16_t marginLeft = 0, const int16_t marginRight = 0) {
  BlockStyle style;
  style.alignment = alignment;
  style.textAlignDefined = true;
  style.marginLeft = marginLeft;
  style.marginRight = marginRight;
  return style;
}

// Mirrors fromCssStyle for a block whose CSS is silent: `alignment` then holds the reader's
// Paragraph Alignment setting (or Justify when that setting is "Book's Style").
BlockStyle undeclared(const CssTextAlign settingAlignment) {
  BlockStyle style;
  style.alignment = settingAlignment;
  style.textAlignDefined = false;
  return style;
}

TEST(BlockStyleAlignedContentX, DeclaredLeftPlacesContentAtTheLeftInset) {
  EXPECT_EQ(declared(CssTextAlign::Left).alignedContentX(VIEWPORT, CONTENT), 0);
}

TEST(BlockStyleAlignedContentX, DeclaredRightPlacesContentAgainstTheBlocksRightEdge) {
  EXPECT_EQ(declared(CssTextAlign::Right).alignedContentX(VIEWPORT, CONTENT), 280);
}

TEST(BlockStyleAlignedContentX, DeclaredCenterSplitsTheRemainingSpace) {
  EXPECT_EQ(declared(CssTextAlign::Center).alignedContentX(VIEWPORT, CONTENT), 140);
}

// Justify and None carry no horizontal intent for placed content.
TEST(BlockStyleAlignedContentX, DeclaredJustifyAndNoneKeepContentCentred) {
  EXPECT_EQ(declared(CssTextAlign::Justify).alignedContentX(VIEWPORT, CONTENT), 140);
  EXPECT_EQ(declared(CssTextAlign::None).alignedContentX(VIEWPORT, CONTENT), 140);
}

// The reader's Paragraph Alignment setting reaches `alignment` whenever the book declares
// nothing. It is a choice about text, so it must not move placed content: a book that asks
// for no alignment renders exactly as it did before alignment was honoured.
TEST(BlockStyleAlignedContentX, UndeclaredAlignmentIgnoresTheReaderSetting) {
  EXPECT_EQ(undeclared(CssTextAlign::Left).alignedContentX(VIEWPORT, CONTENT), 140);
  EXPECT_EQ(undeclared(CssTextAlign::Right).alignedContentX(VIEWPORT, CONTENT), 140);
  EXPECT_EQ(undeclared(CssTextAlign::Justify).alignedContentX(VIEWPORT, CONTENT), 140);
}

// ParsedText rewrites an *undeclared* Left to Right in RTL blocks. Undeclared alignment never
// places content here, so that rule cannot reach this path and RTL blocks stay centred.
TEST(BlockStyleAlignedContentX, RtlBlockWithNoDeclaredAlignmentStaysCentred) {
  BlockStyle style = undeclared(CssTextAlign::Left);
  style.isRtl = true;

  EXPECT_EQ(style.alignedContentX(VIEWPORT, CONTENT), 140);
}

// A declared text-align:left must stay left in an RTL block, for the same CSS-correctness
// reason ParsedText spells out for text.
TEST(BlockStyleAlignedContentX, RtlBlockKeepsDeclaredLeftOnTheLeft) {
  BlockStyle style = declared(CssTextAlign::Left);
  style.isRtl = true;

  EXPECT_EQ(style.alignedContentX(VIEWPORT, CONTENT), style.leftInset());
}

// Asymmetric insets catch the mistake of halving totalHorizontalInset() instead of
// anchoring on leftInset(): both sides differ, so a symmetric shortcut lands elsewhere.
TEST(BlockStyleAlignedContentX, AsymmetricInsetsShiftEveryAlignment) {
  // available = 480 - (40 + 20) = 420
  EXPECT_EQ(declared(CssTextAlign::Left, 40, 20).alignedContentX(VIEWPORT, CONTENT), 40);
  EXPECT_EQ(declared(CssTextAlign::Center, 40, 20).alignedContentX(VIEWPORT, CONTENT), 150);
  EXPECT_EQ(declared(CssTextAlign::Right, 40, 20).alignedContentX(VIEWPORT, CONTENT), 260);
}

// Padding counts toward the inset exactly like margin does.
TEST(BlockStyleAlignedContentX, PaddingCountsTowardTheInset) {
  BlockStyle style = declared(CssTextAlign::Left, 10, 0);
  style.paddingLeft = 30;

  EXPECT_EQ(style.alignedContentX(VIEWPORT, CONTENT), 40);
}

// The case measured on an Xteink X4: a 200px image right-aligned inside a container with
// 40px side margins, on the 454px viewport a default screen margin leaves.
TEST(BlockStyleAlignedContentX, MatchesTheRightAlignedInsetCaseMeasuredOnDevice) {
  // available = 454 - 80 = 374; x = 40 + 374 - 200
  EXPECT_EQ(declared(CssTextAlign::Right, 40, 40).alignedContentX(454, 200), 214);
}

// fromCssStyle caps each side at 2em, but getCombinedBlockStyle adds a parent's insets to its
// child's, so nested blocks can accumulate insets past the viewport. Content must stay on
// screen: the renderer's bounds check drops anything that crosses the edge.
TEST(BlockStyleAlignedContentX, InsetsWiderThanTheViewportKeepContentOnScreen) {
  for (const auto alignment : {CssTextAlign::Left, CssTextAlign::Center, CssTextAlign::Right}) {
    const BlockStyle style = declared(alignment, 300, 300);
    const int16_t x = style.alignedContentX(VIEWPORT, CONTENT);

    EXPECT_GE(x, 0) << "alignment " << static_cast<int>(alignment);
    EXPECT_LE(x + CONTENT, VIEWPORT) << "alignment " << static_cast<int>(alignment);
  }
}

// Content wider than the viewport has nowhere to fit; it starts at the left edge rather than
// at a negative offset.
TEST(BlockStyleAlignedContentX, ContentWiderThanTheViewportStartsAtTheLeftEdge) {
  EXPECT_EQ(declared(CssTextAlign::Center).alignedContentX(VIEWPORT, 600), 0);
}

}  // namespace
