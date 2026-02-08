#include "lib/Epub/Epub/css/CssParser.h"
#include "test/test_harness.h"

// These tests specifically target the code paths changed by the CssParser optimization:
//   1. normalizeInto() replaces normalized() + splitOnChar() in parseDeclarations
//   2. interpretLength() uses strtof directly instead of substring allocations
//   3. interpretSpacing() uses strtof directly instead of substring allocations
//   4. processRuleBlock() uses inline comma scanning instead of splitOnChar

// --- normalizeInto / parseDeclarations regression ---
// The optimization replaced splitOnChar(';') + normalized() per property
// with inline ';'/':' scanning + normalizeInto() into reusable buffers.

void testWhitespaceNormalization() {
  // Leading/trailing whitespace on property names and values
  CssStyle s = CssParser::parseInlineStyle("  font-weight  :  bold  ");
  ASSERT_TRUE(s.hasFontWeight());
  ASSERT_EQ(static_cast<int>(s.fontWeight), static_cast<int>(CssFontWeight::Bold));
}

void testMultipleInternalSpaces() {
  // Multiple spaces between tokens should be collapsed
  CssStyle s = CssParser::parseInlineStyle("text-align:   center");
  ASSERT_TRUE(s.hasTextAlign());
  ASSERT_EQ(static_cast<int>(s.textAlign), static_cast<int>(CssTextAlign::Center));
}

void testTabsAndNewlines() {
  // Tabs and newlines are CSS whitespace, should be normalized
  CssStyle s = CssParser::parseInlineStyle("font-weight:\tbold;\n\ttext-align:\tcenter");
  ASSERT_TRUE(s.hasFontWeight());
  ASSERT_EQ(static_cast<int>(s.fontWeight), static_cast<int>(CssFontWeight::Bold));
  ASSERT_TRUE(s.hasTextAlign());
  ASSERT_EQ(static_cast<int>(s.textAlign), static_cast<int>(CssTextAlign::Center));
}

void testCaseInsensitivity() {
  // Property names and values should be case-insensitive
  CssStyle s1 = CssParser::parseInlineStyle("FONT-WEIGHT: BOLD");
  ASSERT_TRUE(s1.hasFontWeight());
  ASSERT_EQ(static_cast<int>(s1.fontWeight), static_cast<int>(CssFontWeight::Bold));

  CssStyle s2 = CssParser::parseInlineStyle("Text-Align: Center");
  ASSERT_TRUE(s2.hasTextAlign());
  ASSERT_EQ(static_cast<int>(s2.textAlign), static_cast<int>(CssTextAlign::Center));

  CssStyle s3 = CssParser::parseInlineStyle("Font-Style: ITALIC");
  ASSERT_TRUE(s3.hasFontStyle());
  ASSERT_EQ(static_cast<int>(s3.fontStyle), static_cast<int>(CssFontStyle::Italic));
}

void testTrailingSemicolon() {
  // Trailing semicolon should not break parsing
  CssStyle s = CssParser::parseInlineStyle("font-weight: bold;");
  ASSERT_TRUE(s.hasFontWeight());
  ASSERT_EQ(static_cast<int>(s.fontWeight), static_cast<int>(CssFontWeight::Bold));
}

void testDoubleSemicolon() {
  // Double semicolons (empty declaration) should be harmless
  CssStyle s = CssParser::parseInlineStyle("font-weight: bold;; text-align: center");
  ASSERT_TRUE(s.hasFontWeight());
  ASSERT_EQ(static_cast<int>(s.fontWeight), static_cast<int>(CssFontWeight::Bold));
  ASSERT_TRUE(s.hasTextAlign());
  ASSERT_EQ(static_cast<int>(s.textAlign), static_cast<int>(CssTextAlign::Center));
}

void testPropertyWithNoValue() {
  // Missing value after colon should not crash
  CssStyle s = CssParser::parseInlineStyle("font-weight:; text-align: center");
  ASSERT_TRUE(s.hasTextAlign());
  ASSERT_EQ(static_cast<int>(s.textAlign), static_cast<int>(CssTextAlign::Center));
}

void testPropertyWithNoColon() {
  // No colon at all — should be skipped, not crash
  CssStyle s = CssParser::parseInlineStyle("garbage; text-align: center");
  ASSERT_TRUE(s.hasTextAlign());
  ASSERT_EQ(static_cast<int>(s.textAlign), static_cast<int>(CssTextAlign::Center));
}

void testOnlyWhitespace() {
  CssStyle s = CssParser::parseInlineStyle("   \t\n   ");
  ASSERT_FALSE(s.defined.anySet());
}

void testOnlySemicolons() {
  CssStyle s = CssParser::parseInlineStyle(";;;");
  ASSERT_FALSE(s.defined.anySet());
}

// --- interpretLength regression ---
// The optimization replaced substring extraction + stof with direct strtof on the value string.

void testLengthNoUnit() {
  // Bare number should default to Pixels
  CssStyle s = CssParser::parseInlineStyle("margin-top: 10");
  ASSERT_TRUE(s.hasMarginTop());
  ASSERT_NEAR(s.marginTop.value, 10.0f, 0.01f);
  ASSERT_EQ(static_cast<int>(s.marginTop.unit), static_cast<int>(CssUnit::Pixels));
}

void testLengthZero() {
  CssStyle s = CssParser::parseInlineStyle("margin-top: 0");
  ASSERT_TRUE(s.hasMarginTop());
  ASSERT_NEAR(s.marginTop.value, 0.0f, 0.01f);
}

void testLengthZeroPx() {
  CssStyle s = CssParser::parseInlineStyle("margin-top: 0px");
  ASSERT_TRUE(s.hasMarginTop());
  ASSERT_NEAR(s.marginTop.value, 0.0f, 0.01f);
  ASSERT_EQ(static_cast<int>(s.marginTop.unit), static_cast<int>(CssUnit::Pixels));
}

void testLengthNegative() {
  CssStyle s = CssParser::parseInlineStyle("text-indent: -2em");
  ASSERT_TRUE(s.hasTextIndent());
  ASSERT_NEAR(s.textIndent.value, -2.0f, 0.01f);
  ASSERT_EQ(static_cast<int>(s.textIndent.unit), static_cast<int>(CssUnit::Em));
}

void testLengthDecimal() {
  CssStyle s = CssParser::parseInlineStyle("margin-left: 0.5em");
  ASSERT_TRUE(s.hasMarginLeft());
  ASSERT_NEAR(s.marginLeft.value, 0.5f, 0.01f);
  ASSERT_EQ(static_cast<int>(s.marginLeft.unit), static_cast<int>(CssUnit::Em));
}

void testLengthDecimalNoDot() {
  CssStyle s = CssParser::parseInlineStyle("margin-left: 3em");
  ASSERT_TRUE(s.hasMarginLeft());
  ASSERT_NEAR(s.marginLeft.value, 3.0f, 0.01f);
  ASSERT_EQ(static_cast<int>(s.marginLeft.unit), static_cast<int>(CssUnit::Em));
}

void testLengthAllUnits() {
  CssStyle px = CssParser::parseInlineStyle("margin-top: 5px");
  ASSERT_EQ(static_cast<int>(px.marginTop.unit), static_cast<int>(CssUnit::Pixels));
  ASSERT_NEAR(px.marginTop.value, 5.0f, 0.01f);

  CssStyle em = CssParser::parseInlineStyle("margin-top: 2em");
  ASSERT_EQ(static_cast<int>(em.marginTop.unit), static_cast<int>(CssUnit::Em));
  ASSERT_NEAR(em.marginTop.value, 2.0f, 0.01f);

  CssStyle rem = CssParser::parseInlineStyle("margin-top: 1.5rem");
  ASSERT_EQ(static_cast<int>(rem.marginTop.unit), static_cast<int>(CssUnit::Rem));
  ASSERT_NEAR(rem.marginTop.value, 1.5f, 0.01f);

  CssStyle pt = CssParser::parseInlineStyle("margin-top: 12pt");
  ASSERT_EQ(static_cast<int>(pt.marginTop.unit), static_cast<int>(CssUnit::Points));
  ASSERT_NEAR(pt.marginTop.value, 12.0f, 0.01f);
}

void testLengthInvalidValue() {
  // Non-numeric value: property is recognized, value defaults to 0px
  CssStyle s = CssParser::parseInlineStyle("margin-top: abc");
  ASSERT_TRUE(s.hasMarginTop());
  ASSERT_NEAR(s.marginTop.value, 0.0f, 0.01f);
}

// --- interpretSpacing regression ---
// The spacing properties are parsed but not stored in CssStyle (parsed at renderer level).
// Verify that spacing property names don't interfere with other property parsing.

void testSpacingDoesNotCorruptOtherProperties() {
  // letter-spacing and word-spacing are recognized but not stored in CssStyle.
  // Verify they don't break surrounding property parsing.
  CssStyle s = CssParser::parseInlineStyle("font-weight: bold; letter-spacing: 2px; text-align: center");
  ASSERT_TRUE(s.hasFontWeight());
  ASSERT_EQ(static_cast<int>(s.fontWeight), static_cast<int>(CssFontWeight::Bold));
  ASSERT_TRUE(s.hasTextAlign());
  ASSERT_EQ(static_cast<int>(s.textAlign), static_cast<int>(CssTextAlign::Center));
}

void testUnknownPropertySkipped() {
  // Unknown properties should be silently skipped
  CssStyle s = CssParser::parseInlineStyle("color: red; font-weight: bold; display: none");
  ASSERT_TRUE(s.hasFontWeight());
  ASSERT_EQ(static_cast<int>(s.fontWeight), static_cast<int>(CssFontWeight::Bold));
}

// --- processRuleBlock regression ---
// The optimization replaced splitOnChar(',') with inline comma scanning.

void testGroupedSelectorsAfterOptimization() {
  CssParser parser;
  parser.loadFromString("h1, h2, h3 { font-weight: bold }");

  CssStyle h1 = parser.resolveStyle("h1", "");
  CssStyle h2 = parser.resolveStyle("h2", "");
  CssStyle h3 = parser.resolveStyle("h3", "");
  ASSERT_EQ(static_cast<int>(h1.fontWeight), static_cast<int>(CssFontWeight::Bold));
  ASSERT_EQ(static_cast<int>(h2.fontWeight), static_cast<int>(CssFontWeight::Bold));
  ASSERT_EQ(static_cast<int>(h3.fontWeight), static_cast<int>(CssFontWeight::Bold));
}

void testGroupedSelectorsWithClasses() {
  CssParser parser;
  parser.loadFromString("p.intro, p.summary, .note { margin-top: 10px }");

  CssStyle intro = parser.resolveStyle("p", "intro");
  CssStyle summary = parser.resolveStyle("p", "summary");
  CssStyle note = parser.resolveStyle("div", "note");
  ASSERT_NEAR(intro.marginTop.value, 10.0f, 0.01f);
  ASSERT_NEAR(summary.marginTop.value, 10.0f, 0.01f);
  ASSERT_NEAR(note.marginTop.value, 10.0f, 0.01f);
}

void testGroupedSelectorsWhitespaceVariations() {
  // Extra whitespace around commas
  CssParser parser;
  parser.loadFromString("h1 ,  h2  ,h3 { text-align: center }");

  ASSERT_EQ(static_cast<int>(parser.resolveStyle("h1", "").textAlign), static_cast<int>(CssTextAlign::Center));
  ASSERT_EQ(static_cast<int>(parser.resolveStyle("h2", "").textAlign), static_cast<int>(CssTextAlign::Center));
  ASSERT_EQ(static_cast<int>(parser.resolveStyle("h3", "").textAlign), static_cast<int>(CssTextAlign::Center));
}

void testSingleSelectorRuleBlock() {
  // No commas — the inline scanning must handle this correctly
  CssParser parser;
  parser.loadFromString("p { font-style: italic }");

  CssStyle s = parser.resolveStyle("p", "");
  ASSERT_EQ(static_cast<int>(s.fontStyle), static_cast<int>(CssFontStyle::Italic));
}

// --- Compound stress test: many properties, whitespace chaos ---

void testStressMultiplePropertiesWithWhitespace() {
  CssStyle s = CssParser::parseInlineStyle(
      "  font-weight : bold ;  text-align:center;margin-top:10px ; "
      "  margin-bottom : 20px; margin-left:5px ; margin-right : 5px;"
      "padding-top:3px;padding-bottom:  3px; padding-left  : 2em ; "
      "padding-right: 2em ; text-indent : 1.5em;"
      "font-style:italic ;text-decoration :underline ; font-weight: 700");

  ASSERT_EQ(static_cast<int>(s.fontWeight), static_cast<int>(CssFontWeight::Bold));
  ASSERT_EQ(static_cast<int>(s.textAlign), static_cast<int>(CssTextAlign::Center));
  ASSERT_NEAR(s.marginTop.value, 10.0f, 0.01f);
  ASSERT_NEAR(s.marginBottom.value, 20.0f, 0.01f);
  ASSERT_NEAR(s.marginLeft.value, 5.0f, 0.01f);
  ASSERT_NEAR(s.marginRight.value, 5.0f, 0.01f);
  ASSERT_NEAR(s.paddingTop.value, 3.0f, 0.01f);
  ASSERT_NEAR(s.paddingBottom.value, 3.0f, 0.01f);
  ASSERT_NEAR(s.paddingLeft.value, 2.0f, 0.01f);
  ASSERT_NEAR(s.paddingRight.value, 2.0f, 0.01f);
  ASSERT_NEAR(s.textIndent.value, 1.5f, 0.01f);
  ASSERT_EQ(static_cast<int>(s.fontStyle), static_cast<int>(CssFontStyle::Italic));
  ASSERT_EQ(static_cast<int>(s.textDecoration), static_cast<int>(CssTextDecoration::Underline));
}

// --- Repeated calls: ensure no cross-call pollution ---

void testRepeatedCallsNoLeakage() {
  // Call parseInlineStyle many times with different inputs — verify no state leaks
  for (int i = 0; i < 100; ++i) {
    CssStyle bold = CssParser::parseInlineStyle("font-weight: bold");
    ASSERT_EQ(static_cast<int>(bold.fontWeight), static_cast<int>(CssFontWeight::Bold));
    ASSERT_FALSE(bold.hasTextAlign());

    CssStyle center = CssParser::parseInlineStyle("text-align: center");
    ASSERT_EQ(static_cast<int>(center.textAlign), static_cast<int>(CssTextAlign::Center));
    ASSERT_FALSE(center.hasFontWeight());
  }
}

// --- Margin shorthand with different whitespace ---

void testMarginShorthandWhitespace() {
  CssStyle s1 = CssParser::parseInlineStyle("margin:  10px   20px  ");
  ASSERT_NEAR(s1.marginTop.value, 10.0f, 0.01f);
  ASSERT_NEAR(s1.marginRight.value, 20.0f, 0.01f);
  ASSERT_NEAR(s1.marginBottom.value, 10.0f, 0.01f);
  ASSERT_NEAR(s1.marginLeft.value, 20.0f, 0.01f);

  CssStyle s2 = CssParser::parseInlineStyle("margin:1px 2px 3px 4px");
  ASSERT_NEAR(s2.marginTop.value, 1.0f, 0.01f);
  ASSERT_NEAR(s2.marginRight.value, 2.0f, 0.01f);
  ASSERT_NEAR(s2.marginBottom.value, 3.0f, 0.01f);
  ASSERT_NEAR(s2.marginLeft.value, 4.0f, 0.01f);
}

// --- Padding shorthand ---

void testPaddingShorthand() {
  CssStyle s1 = CssParser::parseInlineStyle("padding: 5px");
  ASSERT_NEAR(s1.paddingTop.value, 5.0f, 0.01f);
  ASSERT_NEAR(s1.paddingBottom.value, 5.0f, 0.01f);
  ASSERT_NEAR(s1.paddingLeft.value, 5.0f, 0.01f);
  ASSERT_NEAR(s1.paddingRight.value, 5.0f, 0.01f);

  CssStyle s2 = CssParser::parseInlineStyle("padding: 1px 2px 3px 4px");
  ASSERT_NEAR(s2.paddingTop.value, 1.0f, 0.01f);
  ASSERT_NEAR(s2.paddingRight.value, 2.0f, 0.01f);
  ASSERT_NEAR(s2.paddingBottom.value, 3.0f, 0.01f);
  ASSERT_NEAR(s2.paddingLeft.value, 4.0f, 0.01f);
}

// --- resolveStyle with multiple loadFromString calls ---

void testAccumulatedRules() {
  CssParser parser;
  parser.loadFromString("h1 { text-align: center }");
  parser.loadFromString("p { font-weight: bold }");

  ASSERT_EQ(static_cast<int>(parser.resolveStyle("h1", "").textAlign), static_cast<int>(CssTextAlign::Center));
  ASSERT_EQ(static_cast<int>(parser.resolveStyle("p", "").fontWeight), static_cast<int>(CssFontWeight::Bold));
}

// --- text-decoration-line (alias) ---

void testTextDecorationLine() {
  CssStyle s = CssParser::parseInlineStyle("text-decoration-line: underline");
  ASSERT_TRUE(s.hasTextDecoration());
  ASSERT_EQ(static_cast<int>(s.textDecoration), static_cast<int>(CssTextDecoration::Underline));
}

int main() {
  std::cout << "CssParserRegressionTest\n";

  // normalizeInto / parseDeclarations paths
  RUN_TEST(testWhitespaceNormalization);
  RUN_TEST(testMultipleInternalSpaces);
  RUN_TEST(testTabsAndNewlines);
  RUN_TEST(testCaseInsensitivity);
  RUN_TEST(testTrailingSemicolon);
  RUN_TEST(testDoubleSemicolon);
  RUN_TEST(testPropertyWithNoValue);
  RUN_TEST(testPropertyWithNoColon);
  RUN_TEST(testOnlyWhitespace);
  RUN_TEST(testOnlySemicolons);

  // interpretLength paths
  RUN_TEST(testLengthNoUnit);
  RUN_TEST(testLengthZero);
  RUN_TEST(testLengthZeroPx);
  RUN_TEST(testLengthNegative);
  RUN_TEST(testLengthDecimal);
  RUN_TEST(testLengthDecimalNoDot);
  RUN_TEST(testLengthAllUnits);
  RUN_TEST(testLengthInvalidValue);

  // interpretSpacing paths
  RUN_TEST(testSpacingDoesNotCorruptOtherProperties);
  RUN_TEST(testUnknownPropertySkipped);

  // processRuleBlock paths
  RUN_TEST(testGroupedSelectorsAfterOptimization);
  RUN_TEST(testGroupedSelectorsWithClasses);
  RUN_TEST(testGroupedSelectorsWhitespaceVariations);
  RUN_TEST(testSingleSelectorRuleBlock);

  // Stress / compound
  RUN_TEST(testStressMultiplePropertiesWithWhitespace);
  RUN_TEST(testRepeatedCallsNoLeakage);
  RUN_TEST(testMarginShorthandWhitespace);
  RUN_TEST(testPaddingShorthand);

  // Accumulated rules
  RUN_TEST(testAccumulatedRules);
  RUN_TEST(testTextDecorationLine);

  TEST_SUMMARY();
}
