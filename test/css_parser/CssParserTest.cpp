#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "CssParser.h"

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxRules = 1500;
constexpr size_t kMaxUniqueStyles = 256;
constexpr size_t kCacheHeaderBytes = sizeof(uint8_t) * 2 + sizeof(uint16_t);
constexpr size_t kStyleEnumPrefixBytes = 5;
constexpr size_t kStyleLengthFieldCount = 12;
constexpr size_t kStyleLengthBytes = sizeof(decltype(CssLength::value)) + sizeof(uint8_t);

class CssParserTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    directory_ = fs::temp_directory_path() / "crosspoint_css_parser_test" / info->name();
    fs::remove_all(directory_);
    fs::create_directories(directory_);
    Storage.clearFailures();
  }

  void TearDown() override {
    Storage.clearFailures();
    fs::remove_all(directory_);
  }

  std::string cachePath() const { return directory_.string(); }
  fs::path cacheFile() const { return directory_ / "css_rules.cache"; }
  fs::path cacheTempFile() const { return directory_ / "css_rules.cache.tmp"; }
  fs::path cacheBackupFile() const { return directory_ / "css_rules.cache.bak"; }

  std::vector<uint8_t> readCache() const {
    std::ifstream input(cacheFile(), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  void writeCache(const std::vector<uint8_t>& bytes) const {
    std::ofstream output(cacheFile(), std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  CssParser::ParseResult loadCss(CssParser& parser, const std::string& css) const {
    const fs::path sourcePath = directory_ / "input.css";
    std::ofstream output(sourcePath, std::ios::binary);
    output.write(css.data(), static_cast<std::streamsize>(css.size()));
    output.close();

    HalFile source;
    EXPECT_TRUE(HalStorage::getInstance().openFileForRead("TST", sourcePath.string(), source));
    return parser.loadFromStream(source);
  }

  fs::path directory_;
};

TEST_F(CssParserTest, ResolvesCaseInsensitiveCascadeAndRepeatedSelectors) {
  CssParser parser(cachePath());
  ASSERT_EQ(loadCss(parser,
                    "P { text-align: center; }\n"
                    ".Note { font-weight: bold; text-align: right; }\n"
                    "p.note { font-style: italic; text-align: justify; }\n"
                    ".note { margin-top: 2em; }\n"),
            CssParser::ParseResult::Complete);

  EXPECT_EQ(parser.ruleCount(), 4u);
  const CssStyle style = parser.resolveStyle("p", "NOTE");
  EXPECT_EQ(style.textAlign, CssTextAlign::Justify);
  EXPECT_EQ(style.fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(style.fontStyle, CssFontStyle::Italic);
  ASSERT_TRUE(style.hasMarginTop());
  EXPECT_FLOAT_EQ(style.marginTop.value, 2.0f);
  EXPECT_EQ(style.marginTop.unit, CssUnit::Em);
}

TEST_F(CssParserTest, DeduplicatedStylesReachTheBoundedRuleCap) {
  CssParser parser(cachePath());
  std::string css;
  for (size_t i = 0; i < kMaxRules + 20; ++i) {
    css += ".class-" + std::to_string(i) + " { font-weight: bold; text-indent: 1.5em; }\n";
  }

  EXPECT_EQ(loadCss(parser, css), CssParser::ParseResult::Partial);
  EXPECT_EQ(parser.ruleCount(), kMaxRules);
  EXPECT_EQ(parser.resolveStyle("div", "class-1499").fontWeight, CssFontWeight::Bold);
  EXPECT_FALSE(parser.resolveStyle("div", "class-1500").hasFontWeight());
}

TEST_F(CssParserTest, CascadeUpdatesContinueAfterRuleCap) {
  CssParser parser(cachePath());
  std::string css;
  for (size_t i = 0; i < kMaxRules + 20; ++i) {
    css += ".class-" + std::to_string(i) + " { font-weight: bold; }\n";
  }
  css += ".class-1499 { font-style: italic; }\n";

  EXPECT_EQ(loadCss(parser, css), CssParser::ParseResult::Partial);
  EXPECT_EQ(parser.ruleCount(), kMaxRules);
  const CssStyle style = parser.resolveStyle("div", "class-1499");
  EXPECT_EQ(style.fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(style.fontStyle, CssFontStyle::Italic);
}

TEST_F(CssParserTest, UniqueStyleCapStopsWithoutCorruptingAcceptedRules) {
  CssParser parser(cachePath());
  std::string css;
  for (size_t i = 0; i < kMaxUniqueStyles + 20; ++i) {
    css += ".unique-" + std::to_string(i) + " { text-indent: " + std::to_string(i + 1) + "px; }\n";
  }

  EXPECT_EQ(loadCss(parser, css), CssParser::ParseResult::Partial);
  EXPECT_EQ(parser.ruleCount(), kMaxUniqueStyles);
  EXPECT_FLOAT_EQ(parser.resolveStyle("p", "unique-255").textIndent.value, 256.0f);
  EXPECT_FALSE(parser.resolveStyle("p", "unique-256").hasTextIndent());
}

TEST_F(CssParserTest, RepeatedOverridesReuseAnUnsharedStyleSlot) {
  CssParser parser(cachePath());
  std::string css;
  for (size_t i = 0; i < kMaxUniqueStyles + 20; ++i) {
    css += ".same { text-indent: " + std::to_string(i + 1) + "px; }\n";
  }

  EXPECT_EQ(loadCss(parser, css), CssParser::ParseResult::Complete);
  EXPECT_EQ(parser.ruleCount(), 1u);
  EXPECT_FLOAT_EQ(parser.resolveStyle("p", "same").textIndent.value, static_cast<float>(kMaxUniqueStyles + 20));
}

TEST_F(CssParserTest, OversizedSelectorGroupMarksParsePartialAndSkipsRule) {
  CssParser parser(cachePath());
  const std::string css = "." + std::string(1100, 'a') +
                          " { font-weight: bold; }\n"
                          ".valid { font-style: italic; }\n";

  EXPECT_EQ(loadCss(parser, css), CssParser::ParseResult::Partial);
  EXPECT_EQ(parser.ruleCount(), 1u);
  EXPECT_EQ(parser.resolveStyle("span", "valid").fontStyle, CssFontStyle::Italic);
}

TEST_F(CssParserTest, OversizedDeclarationMarksParsePartialAndKeepsFollowingDeclarations) {
  CssParser parser(cachePath());
  const std::string css = ".long { font-weight: " + std::string(1100, 'x') + "; font-style: italic; }\n";

  EXPECT_EQ(loadCss(parser, css), CssParser::ParseResult::Partial);
  EXPECT_EQ(parser.ruleCount(), 1u);
  const CssStyle style = parser.resolveStyle("span", "long");
  EXPECT_FALSE(style.hasFontWeight());
  EXPECT_EQ(style.fontStyle, CssFontStyle::Italic);
}

TEST_F(CssParserTest, IncompleteInputMarksParsePartial) {
  for (const char* css : {".a { font-weight: bold;", "@media screen {", "/* unfinished", ".unfinished"}) {
    CssParser parser(cachePath());
    EXPECT_EQ(loadCss(parser, css), CssParser::ParseResult::Partial) << css;
  }
}

TEST_F(CssParserTest, CanonicalCacheRoundTripPreservesStyles) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer,
                    "p { text-align: justify; margin-top: 2em; }\n"
                    ".bold { font-weight: bolder; }\n"
                    ".hidden { display: none; }\n"),
            CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(true));
  EXPECT_EQ(writer.inspectCache(), CssParser::CacheStatus::Complete);

  CssParser reader(cachePath());
  ASSERT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Complete);
  EXPECT_EQ(reader.ruleCount(), writer.ruleCount());
  EXPECT_EQ(reader.resolveStyle("span", "bold").fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(reader.resolveStyle("div", "hidden").display, CssDisplay::None);
  const CssStyle paragraph = reader.resolveStyle("p", "");
  EXPECT_EQ(paragraph.textAlign, CssTextAlign::Justify);
  EXPECT_FLOAT_EQ(paragraph.marginTop.value, 2.0f);
}

TEST_F(CssParserTest, PartialCacheIsValidatedDuringInspection) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer, ".a { font-weight: bold; }\n"), CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(false));
  ASSERT_EQ(writer.inspectCache(), CssParser::CacheStatus::Partial);

  const auto size = fs::file_size(cacheFile());
  fs::resize_file(cacheFile(), size - 1);
  EXPECT_EQ(writer.inspectCache(), CssParser::CacheStatus::Invalid);
}

TEST_F(CssParserTest, CompleteCacheDefersPayloadValidationToHydration) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer, ".a { font-weight: bold; }\n"), CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(true));

  const auto size = fs::file_size(cacheFile());
  fs::resize_file(cacheFile(), size - 1);
  EXPECT_EQ(writer.inspectCache(), CssParser::CacheStatus::Complete);

  CssParser reader(cachePath());
  EXPECT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Invalid);
  EXPECT_TRUE(reader.empty());
}

TEST_F(CssParserTest, FailedPromotionRestoresTheOnlyBackupCache) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer, ".original { font-weight: bold; }\n"), CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(true));
  fs::rename(cacheFile(), cacheBackupFile());
  ASSERT_FALSE(fs::exists(cacheFile()));
  ASSERT_EQ(loadCss(writer, ".replacement { font-style: italic; }\n"), CssParser::ParseResult::Complete);

  Storage.failNextRename(cacheTempFile().string(), cacheFile().string());
  EXPECT_FALSE(writer.saveToCache(true));
  EXPECT_TRUE(fs::exists(cacheFile()));
  EXPECT_FALSE(fs::exists(cacheTempFile()));
  EXPECT_FALSE(fs::exists(cacheBackupFile()));

  CssParser reader(cachePath());
  ASSERT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Complete);
  EXPECT_EQ(reader.ruleCount(), 1u);
  EXPECT_EQ(reader.resolveStyle("span", "original").fontWeight, CssFontWeight::Bold);
  EXPECT_FALSE(reader.resolveStyle("span", "replacement").hasFontStyle());
}

TEST_F(CssParserTest, CacheHydrationRejectsInvalidStyleEnumBytes) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer, ".a { font-weight: bold; margin-top: 2em; }\n"), CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(true));

  const std::vector<uint8_t> validCache = readCache();
  ASSERT_GE(validCache.size(), kCacheHeaderBytes + sizeof(uint16_t));
  uint16_t selectorLength = 0;
  memcpy(&selectorLength, validCache.data() + kCacheHeaderBytes, sizeof(selectorLength));
  const size_t styleOffset = kCacheHeaderBytes + sizeof(selectorLength) + selectorLength;

  std::vector<size_t> enumOffsets = {0, 1, 2, 3, 4};
  for (size_t i = 0; i < kStyleLengthFieldCount; ++i) {
    enumOffsets.push_back(kStyleEnumPrefixBytes + i * kStyleLengthBytes + sizeof(decltype(CssLength::value)));
  }
  enumOffsets.push_back(kStyleEnumPrefixBytes + kStyleLengthFieldCount * kStyleLengthBytes);
  enumOffsets.push_back(kStyleEnumPrefixBytes + kStyleLengthFieldCount * kStyleLengthBytes + 1);

  for (const size_t enumOffset : enumOffsets) {
    SCOPED_TRACE(enumOffset);
    std::vector<uint8_t> corruptedCache = validCache;
    ASSERT_LT(styleOffset + enumOffset, corruptedCache.size());
    corruptedCache[styleOffset + enumOffset] = 0xff;
    writeCache(corruptedCache);

    CssParser reader(cachePath());
    EXPECT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Invalid);
    EXPECT_TRUE(reader.empty());
  }
}

TEST_F(CssParserTest, ParsesFontSizeUnitsAndKeywords) {
  CssParser parser(cachePath());
  ASSERT_EQ(loadCss(parser,
                    ".em { font-size: 1.5em; }\n"
                    ".pct { font-size: 80%; }\n"
                    ".px { font-size: 24px; }\n"
                    ".pt { font-size: 18pt; }\n"
                    ".kw { font-size: x-large; }\n"
                    ".rel { font-size: smaller; }\n"
                    ".bad { font-size: 2vw; }\n"),
            CssParser::ParseResult::Complete);

  const auto size = [&](const char* cls) { return parser.resolveStyle("p", cls).fontSize; };
  EXPECT_FLOAT_EQ(size("em").value, 1.5f);
  EXPECT_EQ(size("em").unit, CssUnit::Em);
  EXPECT_FLOAT_EQ(size("pct").value, 0.8f);
  EXPECT_EQ(size("pct").unit, CssUnit::Em);
  EXPECT_FLOAT_EQ(size("px").value, 1.5f);
  EXPECT_EQ(size("px").unit, CssUnit::Rem);
  EXPECT_FLOAT_EQ(size("pt").value, 1.5f);
  EXPECT_FLOAT_EQ(size("kw").value, 1.5f);
  EXPECT_EQ(size("kw").unit, CssUnit::Rem);
  EXPECT_FLOAT_EQ(size("rel").value, 0.83f);
  EXPECT_FALSE(parser.resolveStyle("p", "bad").hasFontSize());
}

TEST_F(CssParserTest, MatchesIdAndCompoundClassSelectors) {
  CssParser parser(cachePath());
  ASSERT_EQ(loadCss(parser,
                    "#title { text-align: center; }\n"
                    "h1#title { font-weight: bold; }\n"
                    ".first.noindent { text-indent: 0; }\n"
                    "p.b.a { font-style: italic; }\n"
                    "*.star { text-align: right; }\n"),
            CssParser::ParseResult::Complete);

  const CssStyle title = parser.resolveStyle("h1", "", "Title");
  EXPECT_EQ(title.textAlign, CssTextAlign::Center);
  EXPECT_EQ(title.fontWeight, CssFontWeight::Bold);
  EXPECT_FALSE(parser.resolveStyle("h2", "", "title").hasFontWeight());

  EXPECT_TRUE(parser.resolveStyle("p", "noindent other first").hasTextIndent());
  EXPECT_FALSE(parser.resolveStyle("p", "first").hasTextIndent());
  EXPECT_EQ(parser.resolveStyle("p", "a b").fontStyle, CssFontStyle::Italic);
  EXPECT_FALSE(parser.resolveStyle("div", "a b").hasFontStyle());
  EXPECT_EQ(parser.resolveStyle("div", "star").textAlign, CssTextAlign::Right);
}

TEST_F(CssParserTest, MatchesDescendantAndChildSelectorsAgainstAncestors) {
  CssParser parser(cachePath());
  ASSERT_EQ(loadCss(parser,
                    "p { text-align: justify; }\n"
                    ".poem p { text-align: left; }\n"
                    "blockquote > p { font-style: italic; }\n"
                    "p + p { font-weight: bold; }\n"
                    "a:hover { font-weight: bold; }\n"),
            CssParser::ParseResult::Complete);
  EXPECT_EQ(parser.ruleCount(), 3u);

  const CssAncestor body = CssParser::makeAncestor("body", "", "");
  const CssAncestor poem = CssParser::makeAncestor("div", "Poem stanza", "");
  const CssAncestor quote = CssParser::makeAncestor("blockquote", "", "");

  const CssAncestor inPoem[] = {body, poem, CssParser::makeAncestor("div", "", "")};
  EXPECT_EQ(parser.resolveStyle("p", "", "", inPoem, 3).textAlign, CssTextAlign::Left);

  const CssAncestor plain[] = {body};
  EXPECT_EQ(parser.resolveStyle("p", "", "", plain, 1).textAlign, CssTextAlign::Justify);
  EXPECT_EQ(parser.resolveStyle("p", "").textAlign, CssTextAlign::Justify);

  const CssAncestor directQuote[] = {body, quote};
  EXPECT_EQ(parser.resolveStyle("p", "", "", directQuote, 2).fontStyle, CssFontStyle::Italic);
  const CssAncestor nestedQuote[] = {body, quote, poem};
  EXPECT_FALSE(parser.resolveStyle("p", "", "", nestedQuote, 3).hasFontStyle());
  EXPECT_FALSE(parser.resolveStyle("p", "", "", nestedQuote, 3).hasFontWeight());
}

TEST_F(CssParserTest, AppliesRulesInSpecificityOrder) {
  CssParser parser(cachePath());
  ASSERT_EQ(loadCss(parser,
                    "#x { text-align: right; }\n"
                    ".c { text-align: center; }\n"
                    "div p { text-align: left; }\n"
                    "p { text-align: justify; }\n"),
            CssParser::ParseResult::Complete);
  const CssAncestor ancestors[] = {CssParser::makeAncestor("div", "", "")};
  EXPECT_EQ(parser.resolveStyle("p", "", "", ancestors, 1).textAlign, CssTextAlign::Left);
  EXPECT_EQ(parser.resolveStyle("p", "c", "", ancestors, 1).textAlign, CssTextAlign::Center);
  EXPECT_EQ(parser.resolveStyle("p", "c", "x", ancestors, 1).textAlign, CssTextAlign::Right);
}

TEST_F(CssParserTest, EqualSpecificityUsesSourceOrderAcrossStylesheetsAndCache) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer, ".z { font-weight: bold; } .a { font-weight: normal; }"), CssParser::ParseResult::Complete);
  for (const char* classes : {"z a", "a z"}) {
    EXPECT_EQ(writer.resolveStyle("p", classes).fontWeight, CssFontWeight::Normal);
  }
  // A later rule for .z must not move its earlier font-weight declaration past .a.
  ASSERT_EQ(loadCss(writer, ".z { font-style: italic; } div p { text-align: right; } body p { text-align: left; }"),
            CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(true));
  CssParser reader(cachePath());
  ASSERT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Complete);
  const CssAncestor ancestors[] = {CssParser::makeAncestor("body", "", ""), CssParser::makeAncestor("div", "", "")};
  for (const CssParser* parser : {&writer, &reader}) {
    for (const char* classes : {"z a", "a z"}) {
      const auto style = parser->resolveStyle("p", classes, "", ancestors, 2);
      EXPECT_EQ(style.fontWeight, CssFontWeight::Normal);
      EXPECT_EQ(style.fontStyle, CssFontStyle::Italic);
      EXPECT_EQ(style.textAlign, CssTextAlign::Left);
    }
  }
  ASSERT_EQ(loadCss(reader, ".z { font-weight: bold; }"), CssParser::ParseResult::Complete);
  for (const char* classes : {"z a", "a z"}) {
    EXPECT_EQ(reader.resolveStyle("p", classes).fontWeight, CssFontWeight::Bold);
  }
}

TEST_F(CssParserTest, RepeatedRulesBeyondOneMatchBatchKeepEarlierProperties) {
  CssParser writer(cachePath());
  std::string css = ".z { font-style: italic; }";
  for (int i = 0; i < 20; ++i) {
    css += ".z { font-weight: bold; } .a { font-weight: normal; }";
  }
  ASSERT_EQ(loadCss(writer, css), CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(true));
  CssParser reader(cachePath());
  ASSERT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Complete);
  for (const CssParser* parser : {&writer, &reader}) {
    for (const char* classes : {"z a", "a z"}) {
      const auto style = parser->resolveStyle("p", classes);
      EXPECT_EQ(style.fontWeight, CssFontWeight::Normal);
      EXPECT_EQ(style.fontStyle, CssFontStyle::Italic);
    }
  }
}

TEST_F(CssParserTest, CacheRoundTripsContextualAndFontSizeRules) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer, ".poem p { font-size: 0.9em; }\n#t.a.b { font-weight: bold; }\n"),
            CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(true));

  CssParser reader(cachePath());
  ASSERT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Complete);
  const CssAncestor ancestors[] = {CssParser::makeAncestor("div", "poem", "")};
  const CssStyle style = reader.resolveStyle("p", "", "", ancestors, 1);
  EXPECT_TRUE(style.hasFontSize());
  EXPECT_FLOAT_EQ(style.fontSize.value, 0.9f);
  EXPECT_EQ(reader.resolveStyle("span", "b a", "t").fontWeight, CssFontWeight::Bold);
}

TEST_F(CssParserTest, ParsesSmallCapsAndForcedPageBreaks) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer,
                    ".sc { font-variant: small-caps; }\n"
                    ".asc { font-variant-caps: all-small-caps; }\n"
                    ".normal { font-variant: normal; }\n"
                    ".before { page-break-before: always; }\n"
                    ".after { break-after: right; }\n"
                    ".avoid { page-break-before: avoid; }\n"),
            CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(true));

  CssParser reader(cachePath());
  ASSERT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Complete);
  EXPECT_TRUE(reader.resolveStyle("span", "sc").smallCaps);
  EXPECT_TRUE(reader.resolveStyle("span", "asc").smallCaps);
  const CssStyle normal = reader.resolveStyle("span", "normal");
  EXPECT_TRUE(normal.hasSmallCaps());
  EXPECT_FALSE(normal.smallCaps);
  EXPECT_TRUE(reader.resolveStyle("div", "before").pageBreakBefore);
  EXPECT_TRUE(reader.resolveStyle("div", "after").pageBreakAfter);
  const CssStyle avoid = reader.resolveStyle("div", "avoid");
  EXPECT_TRUE(avoid.hasPageBreakBefore());
  EXPECT_FALSE(avoid.pageBreakBefore);
}

TEST_F(CssParserTest, ParsesBordersAndBackgroundShade) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer,
                    ".all { border: 1px solid #000; }\n"
                    ".top { border-top: thick double; }\n"
                    ".edges { border-width: 2px 0; border-style: solid; }\n"
                    ".nostyle { border-bottom: 1px; }\n"
                    ".long { border-left-width: 3pt; border-left-style: dashed; }\n"
                    ".gray { background-color: #eee; }\n"
                    ".white { background-color: #fff; }\n"
                    ".dark { background: black; }\n"
                    ".rgb { background: rgb(200, 200, 200) url(x.png) no-repeat; }\n"),
            CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(true));

  CssParser reader(cachePath());
  ASSERT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Complete);
  const CssStyle all = reader.resolveStyle("div", "all");
  EXPECT_TRUE(all.borderTop.visible() && all.borderRight.visible() && all.borderBottom.visible() &&
              all.borderLeft.visible());
  EXPECT_EQ(all.borderLeft.width, 1);
  EXPECT_EQ(all.borderLeft.style, CssBorderStyle::Solid);

  const CssStyle top = reader.resolveStyle("div", "top");
  EXPECT_EQ(top.borderTop.width, 3);
  EXPECT_EQ(top.borderTop.style, CssBorderStyle::Double);
  EXPECT_FALSE(top.borderBottom.visible());

  const CssStyle edges = reader.resolveStyle("div", "edges");
  EXPECT_TRUE(edges.borderTop.visible());
  EXPECT_TRUE(edges.borderBottom.visible());
  EXPECT_FALSE(edges.borderLeft.visible());

  EXPECT_FALSE(reader.resolveStyle("div", "nostyle").hasVisibleBorder());
  const CssStyle dashed = reader.resolveStyle("div", "long");
  EXPECT_EQ(dashed.borderLeft.width, 4);
  EXPECT_EQ(dashed.borderLeft.style, CssBorderStyle::Dashed);

  EXPECT_TRUE(reader.resolveStyle("div", "gray").shaded);
  EXPECT_FALSE(reader.resolveStyle("div", "white").shaded);
  EXPECT_FALSE(reader.resolveStyle("div", "dark").shaded);
  EXPECT_TRUE(reader.resolveStyle("div", "rgb").shaded);
}

TEST_F(CssParserTest, StoresFirstLetterRulesSeparately) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer,
                    "p::first-letter { font-size: 3em; float: left; }\n"
                    ".chapter p:first-letter { initial-letter: 4 3; }\n"
                    "p { text-indent: 1em; }\n"
                    "p::before { content: 'x'; font-weight: bold; }\n"),
            CssParser::ParseResult::Complete);
  EXPECT_TRUE(writer.hasFirstLetterRules());
  ASSERT_TRUE(writer.saveToCache(true));

  CssParser reader(cachePath());
  ASSERT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Complete);
  EXPECT_TRUE(reader.hasFirstLetterRules());
  const CssStyle paragraph = reader.resolveStyle("p", "");
  EXPECT_TRUE(paragraph.hasTextIndent());
  EXPECT_FALSE(paragraph.hasFontSize());
  EXPECT_FALSE(paragraph.hasFontWeight());

  const CssStyle letter = reader.resolveStyle("p", "", "", nullptr, 0, true);
  EXPECT_TRUE(letter.floatLeft);
  EXPECT_FLOAT_EQ(letter.fontSize.value, 3.0f);
  EXPECT_FALSE(letter.hasTextIndent());
  EXPECT_EQ(letter.initialLetter, 0);

  const CssAncestor chapter[] = {CssParser::makeAncestor("div", "chapter", "")};
  EXPECT_EQ(reader.resolveStyle("p", "", "", chapter, 1, true).initialLetter, 4);
}

TEST_F(CssParserTest, CacheHydrationRejectsNonFiniteStyleLengths) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer, ".a { margin-top: 2em; }\n"), CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(true));

  const std::vector<uint8_t> validCache = readCache();
  ASSERT_GE(validCache.size(), kCacheHeaderBytes + sizeof(uint16_t));
  uint16_t selectorLength = 0;
  memcpy(&selectorLength, validCache.data() + kCacheHeaderBytes, sizeof(selectorLength));
  const size_t firstLengthOffset = kCacheHeaderBytes + sizeof(selectorLength) + selectorLength + kStyleEnumPrefixBytes;

  using LengthValue = decltype(CssLength::value);
  for (const LengthValue invalidValue :
       {std::numeric_limits<LengthValue>::quiet_NaN(), std::numeric_limits<LengthValue>::infinity(),
        -std::numeric_limits<LengthValue>::infinity()}) {
    std::vector<uint8_t> corruptedCache = validCache;
    ASSERT_LE(firstLengthOffset + sizeof(invalidValue), corruptedCache.size());
    memcpy(corruptedCache.data() + firstLengthOffset, &invalidValue, sizeof(invalidValue));
    writeCache(corruptedCache);

    CssParser reader(cachePath());
    EXPECT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Invalid);
    EXPECT_TRUE(reader.empty());
  }
}

}  // namespace

TEST_F(CssParserTest, PreservedWhitespaceSurvivesCacheAndNormalOverridesIt) {
  CssParser writer(cachePath());
  ASSERT_EQ(loadCss(writer, ".mono { white-space: pre-wrap; } .normal { white-space: normal; }"),
            CssParser::ParseResult::Complete);
  ASSERT_TRUE(writer.saveToCache(true));
  CssParser reader(cachePath());
  ASSERT_EQ(reader.loadFromCache(), CssParser::CacheLoadResult::Complete);
  auto style = reader.resolveStyle("p", "mono");
  ASSERT_TRUE(style.defined.whiteSpace);
  EXPECT_TRUE(style.preserveWhitespace);
  style.applyOver(reader.resolveStyle("span", "normal"));
  EXPECT_FALSE(style.preserveWhitespace);
}

TEST_F(CssParserTest, WhiteSpacePreservationValuesAndUnsupportedOverrides) {
  for (const char* value : {"pre", "pre-wrap", "break-spaces", "PRE", "BREAK-SPACES !important"}) {
    const std::string declaration = std::string("white-space: ") + value;
    auto style = CssParser::parseInlineStyle(declaration);
    EXPECT_TRUE(style.defined.whiteSpace) << value;
    EXPECT_TRUE(style.preserveWhitespace) << value;
    style.applyOver(CssParser::parseInlineStyle("white-space: normal"));
    EXPECT_TRUE(style.defined.whiteSpace);
    EXPECT_FALSE(style.preserveWhitespace);
  }
  const auto unsupported = CssParser::parseInlineStyle("white-space: nowrap");
  EXPECT_FALSE(unsupported.defined.whiteSpace);
  auto style = CssParser::parseInlineStyle("white-space: pre; white-space: nowrap");
  EXPECT_TRUE(style.defined.whiteSpace);
  EXPECT_TRUE(style.preserveWhitespace);
  style.applyOver(unsupported);
  EXPECT_TRUE(style.preserveWhitespace);
}
