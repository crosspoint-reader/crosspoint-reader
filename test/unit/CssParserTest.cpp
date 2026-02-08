#include "lib/Epub/Epub/css/CssParser.h"
#include "test/test_harness.h"

// --- parseInlineStyle: text properties ---

void testParseInlineTextAlign() {
  CssStyle s = CssParser::parseInlineStyle("text-align: center");
  ASSERT_TRUE(s.hasTextAlign());
  ASSERT_EQ(static_cast<int>(s.textAlign), static_cast<int>(CssTextAlign::Center));
}

void testParseInlineFontStyle() {
  CssStyle s = CssParser::parseInlineStyle("font-style: italic");
  ASSERT_TRUE(s.hasFontStyle());
  ASSERT_EQ(static_cast<int>(s.fontStyle), static_cast<int>(CssFontStyle::Italic));
}

void testParseInlineFontWeight() {
  CssStyle s = CssParser::parseInlineStyle("font-weight: bold");
  ASSERT_TRUE(s.hasFontWeight());
  ASSERT_EQ(static_cast<int>(s.fontWeight), static_cast<int>(CssFontWeight::Bold));
}

void testParseInlineFontWeightNumeric() {
  CssStyle s700 = CssParser::parseInlineStyle("font-weight: 700");
  ASSERT_EQ(static_cast<int>(s700.fontWeight), static_cast<int>(CssFontWeight::Bold));

  CssStyle s400 = CssParser::parseInlineStyle("font-weight: 400");
  ASSERT_EQ(static_cast<int>(s400.fontWeight), static_cast<int>(CssFontWeight::Normal));
}

void testParseInlineTextDecoration() {
  CssStyle s = CssParser::parseInlineStyle("text-decoration: underline");
  ASSERT_TRUE(s.hasTextDecoration());
  ASSERT_EQ(static_cast<int>(s.textDecoration), static_cast<int>(CssTextDecoration::Underline));
}

// --- parseInlineStyle: length values ---

void testParseInlineMarginPx() {
  CssStyle s = CssParser::parseInlineStyle("margin-top: 10px");
  ASSERT_TRUE(s.hasMarginTop());
  ASSERT_NEAR(s.marginTop.value, 10.0f, 0.01f);
  ASSERT_EQ(static_cast<int>(s.marginTop.unit), static_cast<int>(CssUnit::Pixels));
}

void testParseInlineMarginEm() {
  CssStyle s = CssParser::parseInlineStyle("margin-left: 2em");
  ASSERT_TRUE(s.hasMarginLeft());
  ASSERT_NEAR(s.marginLeft.value, 2.0f, 0.01f);
  ASSERT_EQ(static_cast<int>(s.marginLeft.unit), static_cast<int>(CssUnit::Em));
}

void testParseInlineMarginRem() {
  CssStyle s = CssParser::parseInlineStyle("margin-right: 1.5rem");
  ASSERT_TRUE(s.hasMarginRight());
  ASSERT_NEAR(s.marginRight.value, 1.5f, 0.01f);
  ASSERT_EQ(static_cast<int>(s.marginRight.unit), static_cast<int>(CssUnit::Rem));
}

void testParseInlineMarginPt() {
  CssStyle s = CssParser::parseInlineStyle("padding-top: 12pt");
  ASSERT_TRUE(s.hasPaddingTop());
  ASSERT_NEAR(s.paddingTop.value, 12.0f, 0.01f);
  ASSERT_EQ(static_cast<int>(s.paddingTop.unit), static_cast<int>(CssUnit::Points));
}

void testParseInlineTextIndent() {
  CssStyle s = CssParser::parseInlineStyle("text-indent: 1.5em");
  ASSERT_TRUE(s.hasTextIndent());
  ASSERT_NEAR(s.textIndent.value, 1.5f, 0.01f);
}

// --- parseInlineStyle: margin shorthand ---

void testMarginShorthand1Value() {
  CssStyle s = CssParser::parseInlineStyle("margin: 10px");
  ASSERT_NEAR(s.marginTop.value, 10.0f, 0.01f);
  ASSERT_NEAR(s.marginRight.value, 10.0f, 0.01f);
  ASSERT_NEAR(s.marginBottom.value, 10.0f, 0.01f);
  ASSERT_NEAR(s.marginLeft.value, 10.0f, 0.01f);
}

void testMarginShorthand2Values() {
  CssStyle s = CssParser::parseInlineStyle("margin: 10px 20px");
  ASSERT_NEAR(s.marginTop.value, 10.0f, 0.01f);
  ASSERT_NEAR(s.marginRight.value, 20.0f, 0.01f);
  ASSERT_NEAR(s.marginBottom.value, 10.0f, 0.01f);
  ASSERT_NEAR(s.marginLeft.value, 20.0f, 0.01f);
}

void testMarginShorthand4Values() {
  CssStyle s = CssParser::parseInlineStyle("margin: 1px 2px 3px 4px");
  ASSERT_NEAR(s.marginTop.value, 1.0f, 0.01f);
  ASSERT_NEAR(s.marginRight.value, 2.0f, 0.01f);
  ASSERT_NEAR(s.marginBottom.value, 3.0f, 0.01f);
  ASSERT_NEAR(s.marginLeft.value, 4.0f, 0.01f);
}

// --- parseInlineStyle: multiple properties ---

void testMultipleProperties() {
  CssStyle s = CssParser::parseInlineStyle("font-weight: bold; text-align: center; margin-top: 5px");
  ASSERT_TRUE(s.hasFontWeight());
  ASSERT_TRUE(s.hasTextAlign());
  ASSERT_TRUE(s.hasMarginTop());
  ASSERT_EQ(static_cast<int>(s.fontWeight), static_cast<int>(CssFontWeight::Bold));
  ASSERT_EQ(static_cast<int>(s.textAlign), static_cast<int>(CssTextAlign::Center));
  ASSERT_NEAR(s.marginTop.value, 5.0f, 0.01f);
}

void testEmptyStyle() {
  CssStyle s = CssParser::parseInlineStyle("");
  ASSERT_FALSE(s.defined.anySet());
}

// --- CssLength::toPixels ---

void testLengthToPixels() {
  CssLength px{10.0f, CssUnit::Pixels};
  ASSERT_NEAR(px.toPixels(16.0f), 10.0f, 0.01f);

  CssLength em{2.0f, CssUnit::Em};
  ASSERT_NEAR(em.toPixels(16.0f), 32.0f, 0.01f);

  CssLength rem{1.5f, CssUnit::Rem};
  ASSERT_NEAR(rem.toPixels(16.0f), 24.0f, 0.01f);

  CssLength pt{12.0f, CssUnit::Points};
  ASSERT_NEAR(pt.toPixels(16.0f), 15.96f, 0.1f);  // 12 * 1.33
}

// --- CssStyle::applyOver ---

void testApplyOver() {
  CssStyle base;
  base.textAlign = CssTextAlign::Center;
  base.defined.textAlign = 1;
  base.fontWeight = CssFontWeight::Bold;
  base.defined.fontWeight = 1;

  CssStyle target;
  target.fontStyle = CssFontStyle::Italic;
  target.defined.fontStyle = 1;

  target.applyOver(base);
  // base's textAlign and fontWeight should be applied
  ASSERT_EQ(static_cast<int>(target.textAlign), static_cast<int>(CssTextAlign::Center));
  ASSERT_EQ(static_cast<int>(target.fontWeight), static_cast<int>(CssFontWeight::Bold));
  // target's original fontStyle should be preserved
  ASSERT_EQ(static_cast<int>(target.fontStyle), static_cast<int>(CssFontStyle::Italic));
}

void testApplyOverNoOverwrite() {
  CssStyle base;
  // fontWeight not defined in base

  CssStyle target;
  target.fontWeight = CssFontWeight::Bold;
  target.defined.fontWeight = 1;

  target.applyOver(base);
  // target's fontWeight should remain because base didn't define it
  ASSERT_EQ(static_cast<int>(target.fontWeight), static_cast<int>(CssFontWeight::Bold));
}

// --- resolveStyle (requires loadFromString) ---

void testResolveStyleCascade() {
  CssParser parser;
  parser.loadFromString("p { text-align: left } .highlight { font-weight: bold } p.highlight { font-style: italic }");

  CssStyle s = parser.resolveStyle("p", "highlight");
  // Element rule → class rule → combined rule
  ASSERT_EQ(static_cast<int>(s.textAlign), static_cast<int>(CssTextAlign::Left));
  ASSERT_EQ(static_cast<int>(s.fontWeight), static_cast<int>(CssFontWeight::Bold));
  ASSERT_EQ(static_cast<int>(s.fontStyle), static_cast<int>(CssFontStyle::Italic));
}

void testResolveStyleElementOnly() {
  CssParser parser;
  parser.loadFromString("h1 { text-align: center; font-weight: bold }");

  CssStyle s = parser.resolveStyle("h1", "");
  ASSERT_EQ(static_cast<int>(s.textAlign), static_cast<int>(CssTextAlign::Center));
  ASSERT_EQ(static_cast<int>(s.fontWeight), static_cast<int>(CssFontWeight::Bold));
}

void testResolveStyleClassOnly() {
  CssParser parser;
  parser.loadFromString(".intro { margin-top: 20px }");

  CssStyle s = parser.resolveStyle("div", "intro");
  ASSERT_NEAR(s.marginTop.value, 20.0f, 0.01f);
}

void testResolveStyleGroupedSelectors() {
  CssParser parser;
  parser.loadFromString("h1, h2, h3 { font-weight: bold }");

  ASSERT_EQ(static_cast<int>(parser.resolveStyle("h1", "").fontWeight), static_cast<int>(CssFontWeight::Bold));
  ASSERT_EQ(static_cast<int>(parser.resolveStyle("h2", "").fontWeight), static_cast<int>(CssFontWeight::Bold));
  ASSERT_EQ(static_cast<int>(parser.resolveStyle("h3", "").fontWeight), static_cast<int>(CssFontWeight::Bold));
}

void testResolveStyleComments() {
  CssParser parser;
  parser.loadFromString("/* heading styles */ h1 { text-align: center } /* end */");

  CssStyle s = parser.resolveStyle("h1", "");
  ASSERT_EQ(static_cast<int>(s.textAlign), static_cast<int>(CssTextAlign::Center));
}

void testResolveStyleAtRuleSkip() {
  CssParser parser;
  parser.loadFromString("@media screen { p { color: red } } p { font-weight: bold }");

  CssStyle s = parser.resolveStyle("p", "");
  // The @media rule should be skipped, only the plain p rule applies
  ASSERT_EQ(static_cast<int>(s.fontWeight), static_cast<int>(CssFontWeight::Bold));
}

void testResolveStyleNoMatch() {
  CssParser parser;
  parser.loadFromString("h1 { font-weight: bold }");

  CssStyle s = parser.resolveStyle("p", "");
  ASSERT_FALSE(s.defined.anySet());
}

int main() {
  std::cout << "CssParserTest\n";
  RUN_TEST(testParseInlineTextAlign);
  RUN_TEST(testParseInlineFontStyle);
  RUN_TEST(testParseInlineFontWeight);
  RUN_TEST(testParseInlineFontWeightNumeric);
  RUN_TEST(testParseInlineTextDecoration);
  RUN_TEST(testParseInlineMarginPx);
  RUN_TEST(testParseInlineMarginEm);
  RUN_TEST(testParseInlineMarginRem);
  RUN_TEST(testParseInlineMarginPt);
  RUN_TEST(testParseInlineTextIndent);
  RUN_TEST(testMarginShorthand1Value);
  RUN_TEST(testMarginShorthand2Values);
  RUN_TEST(testMarginShorthand4Values);
  RUN_TEST(testMultipleProperties);
  RUN_TEST(testEmptyStyle);
  RUN_TEST(testLengthToPixels);
  RUN_TEST(testApplyOver);
  RUN_TEST(testApplyOverNoOverwrite);
  RUN_TEST(testResolveStyleCascade);
  RUN_TEST(testResolveStyleElementOnly);
  RUN_TEST(testResolveStyleClassOnly);
  RUN_TEST(testResolveStyleGroupedSelectors);
  RUN_TEST(testResolveStyleComments);
  RUN_TEST(testResolveStyleAtRuleSkip);
  RUN_TEST(testResolveStyleNoMatch);
  TEST_SUMMARY();
}
