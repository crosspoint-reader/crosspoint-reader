#include <gtest/gtest.h>

#include <string>

#include "../../lib/Epub/Epub/css/CssParser.h"

TEST(CssParserDeclarations, InlineLineThrough) {
  const CssStyle style = CssParser::parseInlineStyle("text-decoration: line-through");
  ASSERT_TRUE(style.hasTextDecoration());
  ASSERT_EQ(style.textDecoration, CssTextDecoration::LineThrough);
}

TEST(CssParserDeclarations, InlineUnderlineLineThrough) {
  const CssStyle style = CssParser::parseInlineStyle("text-decoration: underline line-through");
  ASSERT_TRUE(style.hasTextDecoration());
  ASSERT_EQ(style.textDecoration, CssTextDecoration::UnderlineLineThrough);
}

TEST(CssParserDeclarations, InlineLineThroughUnderlineOrderInsensitive) {
  const CssStyle style = CssParser::parseInlineStyle("text-decoration: line-through underline");
  ASSERT_TRUE(style.hasTextDecoration());
  ASSERT_EQ(style.textDecoration, CssTextDecoration::UnderlineLineThrough);
}

TEST(CssParserDeclarations, InlineTextDecorationNormalization) {
  const CssStyle style = CssParser::parseInlineStyle("TEXT-DECORATION : LINE-THROUGH ;");
  ASSERT_TRUE(style.hasTextDecoration());
  ASSERT_EQ(style.textDecoration, CssTextDecoration::LineThrough);
}

// `img { height: 100%; ...; height: auto; width: 100% }` — the later `auto` has to win. Dropping
// it as unparseable left the 100% standing, which sized the image box to the whole viewport while
// the decoder filled only the aspect-correct top of it; the rest replayed as a black band.
TEST(CssParserDeclarations, ImageHeightAutoClearsEarlierLength) {
  const CssStyle style = CssParser::parseInlineStyle("height: 100%; width: 100%; height: auto");
  ASSERT_TRUE(style.hasImageWidth());
  ASSERT_FALSE(style.hasImageHeight());
}

// Order still matters the other way round: a length after the auto wins.
TEST(CssParserDeclarations, ImageHeightLengthAfterAutoWins) {
  const CssStyle style = CssParser::parseInlineStyle("height: auto; height: 2em");
  ASSERT_TRUE(style.hasImageHeight());
  ASSERT_EQ(style.imageHeight.unit, CssUnit::Em);
}

// The auto marker must survive the cascade merge, so a more specific rule saying `width: auto`
// clears a length a less specific one set.
TEST(CssParserDeclarations, ImageWidthAutoOverridesLowerPriorityRule) {
  CssStyle resolved = CssParser::parseInlineStyle("width: 50%");
  resolved.applyOver(CssParser::parseInlineStyle("width: auto"));
  ASSERT_FALSE(resolved.hasImageWidth());
}

// Unsupported keywords stay ignored — only auto and the CSS-wide keywords that resolve to it
// clear the property.
TEST(CssParserDeclarations, ImageHeightUnknownKeywordIgnored) {
  const CssStyle style = CssParser::parseInlineStyle("height: 100%; height: fit-content");
  ASSERT_TRUE(style.hasImageHeight());
  ASSERT_EQ(style.imageHeight.unit, CssUnit::Percent);
}

TEST(CssParserDeclarations, ImageWidthImportant) {
  const CssStyle style = CssParser::parseInlineStyle("width: 50% !important");
  ASSERT_TRUE(style.hasImageWidth());
  ASSERT_EQ(style.imageWidth.unit, CssUnit::Percent);
  ASSERT_EQ(style.imageWidth.value, 50.0f);
}

// Issue #388: every value but `none` used to read as Block, so a <span> styled display:inline
// could not be told from one styled display:block -- and the parser lays out only the latter
// as a block. Block-level values are Block; every other shown value is Inline.
TEST(CssParserDeclarations, DisplayKeepsBlockAndInlineApart) {
  const struct {
    const char* declaration;
    CssDisplay want;
  } cases[] = {
      {"display: block", CssDisplay::Block},         {"display: list-item", CssDisplay::Block},
      {"display: table", CssDisplay::Block},         {"display: inline", CssDisplay::Inline},
      {"display: inline-block", CssDisplay::Inline}, {"display: table-cell", CssDisplay::Inline},
      {"display: none", CssDisplay::None},           {"DISPLAY: BLOCK !important", CssDisplay::Block},
  };
  for (const auto& c : cases) {
    const CssStyle style = CssParser::parseInlineStyle(c.declaration);
    ASSERT_TRUE(style.hasDisplay()) << c.declaration;
    EXPECT_EQ(static_cast<int>(style.display), static_cast<int>(c.want)) << c.declaration;
  }
}

// Regression: `!important` used to be stripped only by the dozen properties that called
// stripTrailingImportant themselves. Everything else compared the marker as part of the value
// and silently dropped the declaration. crosspoint-reader PR #3221.
TEST(CssParserDeclarations, ImportantStrippedForEveryProperty) {
  const CssStyle align = CssParser::parseInlineStyle("text-align: center !important");
  ASSERT_TRUE(align.hasTextAlign());
  ASSERT_EQ(static_cast<int>(align.textAlign), static_cast<int>(CssTextAlign::Center));

  const CssStyle weight = CssParser::parseInlineStyle("font-weight: bold !important");
  ASSERT_TRUE(weight.hasFontWeight());
  ASSERT_EQ(static_cast<int>(weight.fontWeight), static_cast<int>(CssFontWeight::Bold));

  const CssStyle decoration = CssParser::parseInlineStyle("text-decoration: underline !important");
  ASSERT_TRUE(decoration.hasTextDecoration());
  ASSERT_EQ(static_cast<int>(decoration.textDecoration), static_cast<int>(CssTextDecoration::Underline));

  const CssStyle indent = CssParser::parseInlineStyle("text-indent: 2em !important");
  ASSERT_TRUE(indent.hasTextIndent());
  ASSERT_EQ(indent.textIndent.value, 2.0f);

  // Shorthand: the marker has to go before the value is split on whitespace, or the last
  // component parses as "0 !important" and the whole declaration is dropped.
  const CssStyle margin = CssParser::parseInlineStyle("margin: 0 !important");
  ASSERT_TRUE(margin.hasMarginTop());
  ASSERT_EQ(margin.marginTop.value, 0.0f);
}

// Font-size absolute units and keywords resolve to body-relative multipliers:
// pt normalises against 12 pt, px against 16 px, keywords use fixed steps.
TEST(CssParserUnits, FontSizeKeywordsAndAbsoluteUnits) {
  struct Case {
    const char* decl;
    float expected;
  };
  const Case cases[] = {
      {"font-size: 9pt", 0.75f},     {"font-size: 12pt", 1.0f},     {"font-size: 24pt", 2.0f},
      {"font-size: 8px", 0.5f},      {"font-size: 16px", 1.0f},     {"font-size: 32px", 2.0f},
      {"font-size: xx-small", 0.6f}, {"font-size: x-small", 0.75f}, {"font-size: small", 0.8f},
      {"font-size: smaller", 0.8f},  {"font-size: medium", 1.0f},   {"font-size: large", 1.2f},
      {"font-size: larger", 1.2f},   {"font-size: x-large", 1.4f},  {"font-size: xx-large", 1.6f},
  };
  for (const auto& c : cases) {
    const CssStyle st = CssParser::parseInlineStyle(c.decl);
    EXPECT_TRUE(st.hasFontSizeMultiplier()) << c.decl;
    EXPECT_FLOAT_EQ(st.fontSizeMultiplier, c.expected) << c.decl;
  }

  // Unknown keyword must leave font-size undefined.
  const CssStyle bogus = CssParser::parseInlineStyle("font-size: enormous");
  EXPECT_FALSE(bogus.hasFontSizeMultiplier());
}

// em, % and smaller/larger scale the parent's size; rem, px, pt and the absolute keywords are the
// body's. The parser composes the first kind and starts over from the body for the second.
TEST(CssParserUnits, FontSizeKnowsWhatItIsRelativeTo) {
  for (const char* decl : {"font-size: 1.2em", "font-size: 80%", "font-size: smaller", "font-size: larger"}) {
    const CssStyle st = CssParser::parseInlineStyle(decl);
    EXPECT_TRUE(st.hasFontSizeMultiplier()) << decl;
    EXPECT_FALSE(st.fontSizeRootRelative) << decl;
  }
  for (const char* decl :
       {"font-size: 1.2rem", "font-size: 12pt", "font-size: 20px", "font-size: small", "font-size: medium"}) {
    const CssStyle st = CssParser::parseInlineStyle(decl);
    EXPECT_TRUE(st.hasFontSizeMultiplier()) << decl;
    EXPECT_TRUE(st.fontSizeRootRelative) << decl;
  }
  // The flag travels with the size through the cascade.
  CssStyle resolved = CssParser::parseInlineStyle("font-size: 1rem");
  resolved.applyOver(CssParser::parseInlineStyle("font-size: 0.9em"));
  EXPECT_FALSE(resolved.fontSizeRootRelative);
  resolved.applyOver(CssParser::parseInlineStyle("font-size: 14px"));
  EXPECT_TRUE(resolved.fontSizeRootRelative);
}

// A watermark span's `font-size: 1px` must read as invisible text; nothing meant to be read may.
// The line sits at a tenth of the body (CssStyle::kInvisibleFontSizeMultiplier), 1.6 px of 16 px.
TEST(CssParserUnits, FontSizeInvisibleOnlyBelowReadableSizes) {
  const char* const invisible[] = {"font-size: 1px",    "font-size: 0.5px",   "font-size: 1.5px", "font-size: 1pt",
                                   "font-size: 0.09em", "font-size: 0.05rem", "font-size: 9%"};
  for (const char* decl : invisible) {
    EXPECT_TRUE(CssParser::parseInlineStyle(decl).isFontSizeInvisible()) << decl;
  }

  // 10 % is the renderer's clamp case (test_font_sizes.epub): raised to the 30 % floor, not hidden.
  const char* const readable[] = {"font-size: 10%",   "font-size: 0.1em", "font-size: 2px",      "font-size: 2pt",
                                  "font-size: 0.2em", "font-size: 0.5em", "font-size: xx-small", "font-size: smaller",
                                  "font-size: 1em",   "color: white",     "font-size: inherit"};
  for (const char* decl : readable) {
    EXPECT_FALSE(CssParser::parseInlineStyle(decl).isFontSizeInvisible()) << decl;
  }

  // Zero is not a size the parser records (it stays inherited), so it is not invisible either:
  // the zero-size container idiom relies on its children re-declaring a size.
  EXPECT_FALSE(CssParser::parseInlineStyle("font-size: 0").isFontSizeInvisible());
  EXPECT_FALSE(CssParser::parseInlineStyle("font-size: 0px").isFontSizeInvisible());
}

// auto / avoid / avoid-page are an explicit "no break", kept so they can cancel an `always` from a
// rule they outrank; values that are not about pages leave the property unset.
TEST(CssParserDeclarations, PageBreakAvoidIsAnExplicitNoBreak) {
  for (const char* decl :
       {"page-break-before: always", "break-before: page", "page-break-before: left", "break-before: recto"}) {
    const CssStyle style = CssParser::parseInlineStyle(decl);
    EXPECT_TRUE(style.hasPageBreakBefore() && style.pageBreakBefore) << decl;
  }
  for (const char* decl : {"page-break-before: auto", "page-break-before: avoid", "break-before: avoid-page",
                           "page-break-before: always; page-break-before: avoid"}) {
    const CssStyle style = CssParser::parseInlineStyle(decl);
    EXPECT_TRUE(style.hasPageBreakBefore()) << decl;
    EXPECT_FALSE(style.pageBreakBefore) << decl;
  }
  for (const char* decl : {"break-after: column", "page-break-after: inherit"}) {
    EXPECT_FALSE(CssParser::parseInlineStyle(decl).hasPageBreakAfter()) << decl;
  }
  const CssStyle after = CssParser::parseInlineStyle("page-break-after: avoid");
  EXPECT_TRUE(after.hasPageBreakAfter());
  EXPECT_FALSE(after.pageBreakAfter);

  // An inline `avoid` cancels the stylesheet's `always`.
  CssStyle resolved = CssParser::parseInlineStyle("page-break-before: always");
  resolved.applyOver(CssParser::parseInlineStyle("page-break-before: auto"));
  EXPECT_FALSE(resolved.pageBreakBefore);
}

// `inherit` leaves the element to its parent: unset to the parser (has*() false), yet defined, so
// it beats a lower rule's value in the cascade. It used to read as normal / left / none, which
// cancelled the bold, italic, alignment or underline the element sat in.
TEST(CssParserDeclarations, InheritDefersToTheParent) {
  const CssStyle style = CssParser::parseInlineStyle("font-weight: inherit; font-style: inherit; text-align: inherit");
  EXPECT_FALSE(style.hasFontWeight());
  EXPECT_FALSE(style.hasFontStyle());
  EXPECT_FALSE(style.hasTextAlign());
  EXPECT_TRUE(style.defined.fontWeight && style.defined.fontStyle && style.defined.textAlign);

  EXPECT_FALSE(CssParser::parseInlineStyle("text-decoration: inherit").hasTextDecoration());
  EXPECT_FALSE(CssParser::parseInlineStyle("font-weight: bold; font-weight: inherit").hasFontWeight());
  const CssStyle later = CssParser::parseInlineStyle("font-weight: inherit; font-weight: bold");
  EXPECT_TRUE(later.hasFontWeight());
  EXPECT_EQ(later.fontWeight, CssFontWeight::Bold);
  // Explicit normal still resets.
  const CssStyle normal = CssParser::parseInlineStyle("font-style: normal");
  EXPECT_TRUE(normal.hasFontStyle());
  EXPECT_EQ(normal.fontStyle, CssFontStyle::Normal);

  // A more specific (here: inline) inherit cancels a stylesheet's bold.
  CssStyle resolved = CssParser::parseInlineStyle("font-weight: bold; text-align: center");
  resolved.applyOver(CssParser::parseInlineStyle("font-weight: inherit; text-align: inherit"));
  EXPECT_FALSE(resolved.hasFontWeight());
  EXPECT_FALSE(resolved.hasTextAlign());
}
