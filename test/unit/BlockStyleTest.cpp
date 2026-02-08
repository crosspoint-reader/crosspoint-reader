#include "lib/Epub/Epub/blocks/BlockStyle.h"
#include "test/test_harness.h"

// --- fromCssStyle ---

void testFromCssStylePixels() {
  CssStyle css;
  css.marginTop = CssLength{10.0f, CssUnit::Pixels};
  css.marginBottom = CssLength{20.0f, CssUnit::Pixels};
  css.marginLeft = CssLength{5.0f, CssUnit::Pixels};
  css.marginRight = CssLength{5.0f, CssUnit::Pixels};
  css.textIndent = CssLength{15.0f, CssUnit::Pixels};
  css.defined.marginTop = css.defined.marginBottom = 1;
  css.defined.marginLeft = css.defined.marginRight = 1;
  css.defined.textIndent = 1;

  BlockStyle bs = BlockStyle::fromCssStyle(css, 16.0f, CssTextAlign::None);
  ASSERT_EQ(bs.marginTop, 10);
  ASSERT_EQ(bs.marginBottom, 20);
  ASSERT_EQ(bs.marginLeft, 5);
  ASSERT_EQ(bs.marginRight, 5);
  ASSERT_EQ(bs.textIndent, 15);
  ASSERT_TRUE(bs.textIndentDefined);
}

void testFromCssStyleEm() {
  CssStyle css;
  css.marginTop = CssLength{1.0f, CssUnit::Em};
  css.defined.marginTop = 1;

  BlockStyle bs = BlockStyle::fromCssStyle(css, 20.0f, CssTextAlign::None);
  ASSERT_EQ(bs.marginTop, 20);  // 1em * 20px
}

void testFromCssStylePoints() {
  CssStyle css;
  css.paddingLeft = CssLength{10.0f, CssUnit::Points};
  css.defined.paddingLeft = 1;

  BlockStyle bs = BlockStyle::fromCssStyle(css, 16.0f, CssTextAlign::None);
  ASSERT_EQ(bs.paddingLeft, 13);  // 10pt * 1.33 = 13.3 → 13
}

void testFromCssStyleAlignmentOverride() {
  CssStyle css;
  css.textAlign = CssTextAlign::Center;
  css.defined.textAlign = 1;

  // User preference Left overrides CSS Center
  BlockStyle bs = BlockStyle::fromCssStyle(css, 16.0f, CssTextAlign::Left);
  ASSERT_EQ(static_cast<int>(bs.alignment), static_cast<int>(CssTextAlign::Left));
  ASSERT_TRUE(bs.textAlignDefined);
}

void testFromCssStyleAlignmentBookStyle() {
  CssStyle css;
  css.textAlign = CssTextAlign::Center;
  css.defined.textAlign = 1;

  // CssTextAlign::None = "Book's Style" → use CSS value
  BlockStyle bs = BlockStyle::fromCssStyle(css, 16.0f, CssTextAlign::None);
  ASSERT_EQ(static_cast<int>(bs.alignment), static_cast<int>(CssTextAlign::Center));
}

void testFromCssStyleNoAlignDefined() {
  CssStyle css;
  // text-align not defined, and user uses "Book's Style"
  BlockStyle bs = BlockStyle::fromCssStyle(css, 16.0f, CssTextAlign::None);
  // Default should be Justify when no CSS alignment is set
  ASSERT_EQ(static_cast<int>(bs.alignment), static_cast<int>(CssTextAlign::Justify));
  ASSERT_FALSE(bs.textAlignDefined);
}

// --- getCombinedBlockStyle ---

void testCombinedMarginsAdd() {
  BlockStyle parent;
  parent.marginLeft = 10;
  parent.paddingLeft = 5;

  BlockStyle child;
  child.marginLeft = 8;
  child.paddingLeft = 3;

  BlockStyle combined = parent.getCombinedBlockStyle(child);
  ASSERT_EQ(combined.marginLeft, 18);
  ASSERT_EQ(combined.paddingLeft, 8);
}

void testCombinedTextIndentChild() {
  BlockStyle parent;
  parent.textIndent = 20;
  parent.textIndentDefined = true;

  BlockStyle child;
  child.textIndent = 10;
  child.textIndentDefined = true;

  BlockStyle combined = parent.getCombinedBlockStyle(child);
  ASSERT_EQ(combined.textIndent, 10);  // child's textIndent wins
  ASSERT_TRUE(combined.textIndentDefined);
}

void testCombinedTextIndentParentOnly() {
  BlockStyle parent;
  parent.textIndent = 20;
  parent.textIndentDefined = true;

  BlockStyle child;
  // child doesn't define textIndent

  BlockStyle combined = parent.getCombinedBlockStyle(child);
  ASSERT_EQ(combined.textIndent, 20);  // parent's textIndent preserved
  ASSERT_TRUE(combined.textIndentDefined);
}

void testCombinedAlignmentChild() {
  BlockStyle parent;
  parent.alignment = CssTextAlign::Left;
  parent.textAlignDefined = true;

  BlockStyle child;
  child.alignment = CssTextAlign::Center;
  child.textAlignDefined = true;

  BlockStyle combined = parent.getCombinedBlockStyle(child);
  ASSERT_EQ(static_cast<int>(combined.alignment), static_cast<int>(CssTextAlign::Center));
}

// --- inset helpers ---

void testInsets() {
  BlockStyle bs;
  bs.marginLeft = 10;
  bs.paddingLeft = 5;
  bs.marginRight = 8;
  bs.paddingRight = 3;

  ASSERT_EQ(bs.leftInset(), 15);
  ASSERT_EQ(bs.rightInset(), 11);
  ASSERT_EQ(bs.totalHorizontalInset(), 26);
}

int main() {
  std::cout << "BlockStyleTest\n";
  RUN_TEST(testFromCssStylePixels);
  RUN_TEST(testFromCssStyleEm);
  RUN_TEST(testFromCssStylePoints);
  RUN_TEST(testFromCssStyleAlignmentOverride);
  RUN_TEST(testFromCssStyleAlignmentBookStyle);
  RUN_TEST(testFromCssStyleNoAlignDefined);
  RUN_TEST(testCombinedMarginsAdd);
  RUN_TEST(testCombinedTextIndentChild);
  RUN_TEST(testCombinedTextIndentParentOnly);
  RUN_TEST(testCombinedAlignmentChild);
  RUN_TEST(testInsets);
  TEST_SUMMARY();
}
