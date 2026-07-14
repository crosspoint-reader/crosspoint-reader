#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "CssParser.h"

namespace fs = std::filesystem;

namespace {

// Behavioral limits mirrored from CssParser.cpp. If the caps change there,
// update the expectations here.
constexpr size_t kMaxRules = 2048;
constexpr size_t kMaxUniqueStyles = 256;
constexpr size_t kStyleWireBytes = 66;

class CssParserTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() / "crosspoint_css_parser_test" / info->name();
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }

  void TearDown() override { fs::remove_all(dir_); }

  std::string cachePath() const { return dir_.string(); }
  std::string cacheFilePath() const { return (dir_ / "css_rules.cache").string(); }

  void writeFile(const fs::path& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary);
    out << contents;
  }

  void loadCss(CssParser& parser, const std::string& css) {
    const fs::path cssPath = dir_ / "input.css";
    writeFile(cssPath, css);
    HalFile file;
    ASSERT_TRUE(HalStorage::getInstance().openFileForRead("TST", cssPath.string(), file));
    ASSERT_TRUE(parser.loadFromStream(file));
  }

  fs::path dir_;
};

TEST_F(CssParserTest, CascadeOrderTagClassTagClass) {
  CssParser parser(cachePath());
  loadCss(parser,
          "p { text-align: center; }\n"
          ".cls { font-weight: bold; text-align: right; }\n"
          "p.cls { text-align: justify; }\n");

  const CssStyle full = parser.resolveStyle("p", "cls");
  EXPECT_EQ(full.textAlign, CssTextAlign::Justify);
  EXPECT_EQ(full.fontWeight, CssFontWeight::Bold);

  const CssStyle tagOnly = parser.resolveStyle("p", "");
  EXPECT_EQ(tagOnly.textAlign, CssTextAlign::Center);
  EXPECT_FALSE(tagOnly.hasFontWeight());

  const CssStyle classOnly = parser.resolveStyle("div", "cls");
  EXPECT_EQ(classOnly.textAlign, CssTextAlign::Right);
  EXPECT_EQ(classOnly.fontWeight, CssFontWeight::Bold);
}

TEST_F(CssParserTest, BolderKeywordMapsToBold) {
  // Regression for issue #2591's book: rules use `font-weight: bolder`.
  CssParser parser(cachePath());
  loadCss(parser, ".class-0-1193 { font-style : normal ; font-weight : bolder ; text-indent : 1.5em ; }\n");

  const CssStyle style = parser.resolveStyle("div", "class-0-1193");
  EXPECT_TRUE(style.hasFontWeight());
  EXPECT_EQ(style.fontWeight, CssFontWeight::Bold);
  EXPECT_TRUE(style.hasTextIndent());
  EXPECT_FLOAT_EQ(style.textIndent.value, 1.5f);
  EXPECT_EQ(style.textIndent.unit, CssUnit::Em);
}

TEST_F(CssParserTest, SelectorsAreCaseInsensitive) {
  CssParser parser(cachePath());
  loadCss(parser,
          ".FOO { font-weight: bold; }\n"
          "DIV { font-style: italic; }\n");

  EXPECT_EQ(parser.resolveStyle("p", "foo").fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(parser.resolveStyle("P", "FoO").fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(parser.resolveStyle("div", "").fontStyle, CssFontStyle::Italic);
  EXPECT_EQ(parser.resolveStyle("DIV", "").fontStyle, CssFontStyle::Italic);
  // .FOO and .foo fold to the same selector
  EXPECT_EQ(parser.ruleCount(), 2u);
}

TEST_F(CssParserTest, DuplicateSelectorAcrossStreamsMergesLaterWins) {
  CssParser parser(cachePath());
  loadCss(parser, ".a { font-weight: bold; text-align: left; }\n");
  loadCss(parser, ".a { font-style: italic; text-align: right; }\n");

  EXPECT_EQ(parser.ruleCount(), 1u);
  const CssStyle style = parser.resolveStyle("p", "a");
  EXPECT_EQ(style.fontWeight, CssFontWeight::Bold);  // kept from first stream
  EXPECT_EQ(style.fontStyle, CssFontStyle::Italic);  // added by second stream
  EXPECT_EQ(style.textAlign, CssTextAlign::Right);   // later declaration wins
}

TEST_F(CssParserTest, CommaGroupCreatesEntriesSharingOneStyle) {
  CssParser parser(cachePath());
  loadCss(parser, "h1, h2, .x { font-weight: bold; }\n");

  EXPECT_EQ(parser.ruleCount(), 3u);
  EXPECT_EQ(parser.uniqueStyleCount(), 1u);
  EXPECT_EQ(parser.resolveStyle("h1", "").fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(parser.resolveStyle("h2", "").fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(parser.resolveStyle("p", "x").fontWeight, CssFontWeight::Bold);
}

TEST_F(CssParserTest, IdenticalBodiesDeduplicateAcrossSelectors) {
  CssParser parser(cachePath());
  std::string css;
  for (int i = 0; i < 100; ++i) {
    css += ".c" + std::to_string(i) + " { font-weight: bold; text-indent: 1.5em; }\n";
  }
  css += ".other { font-style: italic; }\n";
  loadCss(parser, css);

  EXPECT_EQ(parser.ruleCount(), 101u);
  EXPECT_EQ(parser.uniqueStyleCount(), 2u);
}

TEST_F(CssParserTest, UnsupportedSelectorsAreSkipped) {
  CssParser parser(cachePath());
  loadCss(parser,
          "p > .x { font-weight: bold; }\n"
          "#id { font-weight: bold; }\n"
          ".a:hover { font-weight: bold; }\n"
          "ul li { font-weight: bold; }\n"
          "* { font-weight: bold; }\n");
  EXPECT_EQ(parser.ruleCount(), 0u);
}

TEST_F(CssParserTest, SaveLoadRoundTrip) {
  CssParser writer(cachePath());
  loadCss(writer,
          "p { text-align: justify; margin-top: 2em; }\n"
          ".bold { font-weight: bolder; }\n"
          ".italic { font-style: italic; }\n"
          "p.deco { text-decoration: underline; }\n");
  ASSERT_TRUE(writer.saveToCache());

  CssParser reader(cachePath());
  ASSERT_TRUE(reader.hasCache());
  ASSERT_TRUE(reader.loadFromCache());

  EXPECT_EQ(reader.ruleCount(), writer.ruleCount());
  EXPECT_EQ(reader.uniqueStyleCount(), writer.uniqueStyleCount());

  const CssStyle p = reader.resolveStyle("p", "");
  EXPECT_EQ(p.textAlign, CssTextAlign::Justify);
  EXPECT_TRUE(p.hasMarginTop());
  EXPECT_FLOAT_EQ(p.marginTop.value, 2.0f);
  EXPECT_EQ(p.marginTop.unit, CssUnit::Em);
  EXPECT_EQ(reader.resolveStyle("span", "bold").fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(reader.resolveStyle("span", "italic").fontStyle, CssFontStyle::Italic);
  EXPECT_EQ(reader.resolveStyle("p", "deco").textDecoration, CssTextDecoration::Underline);
}

TEST_F(CssParserTest, EmptyRuleSetRoundTrips) {
  CssParser writer(cachePath());
  ASSERT_TRUE(writer.saveToCache());

  CssParser reader(cachePath());
  ASSERT_TRUE(reader.loadFromCache());
  EXPECT_EQ(reader.ruleCount(), 0u);
  EXPECT_TRUE(reader.empty());
}

TEST_F(CssParserTest, OldCacheVersionRejectedAndDeleted) {
  // A v7 (or any wrong-version) cache must be rejected and removed so the
  // caller re-parses; this is what self-heals devices with truncated caches.
  std::string stale;
  stale.push_back(static_cast<char>(7));
  stale += "arbitrary v7 payload bytes";
  writeFile(cacheFilePath(), stale);

  CssParser parser(cachePath());
  EXPECT_FALSE(parser.loadFromCache());
  EXPECT_FALSE(parser.hasCache());
}

TEST_F(CssParserTest, TruncatedCacheRejected) {
  CssParser writer(cachePath());
  loadCss(writer, ".a { font-weight: bold; }\n.b { font-style: italic; }\n");
  ASSERT_TRUE(writer.saveToCache());

  // Chop the last 4 bytes: payload no longer matches the header.
  const auto size = fs::file_size(cacheFilePath());
  fs::resize_file(cacheFilePath(), size - 4);

  CssParser reader(cachePath());
  EXPECT_FALSE(reader.loadFromCache());
  EXPECT_EQ(reader.ruleCount(), 0u);
}

TEST_F(CssParserTest, UnsortedCacheEntriesRejected) {
  // Hand-craft a v8 cache whose two entries are in descending selector order.
  std::vector<uint8_t> buf;
  auto push16 = [&buf](uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>(v >> 8));
  };
  auto push32 = [&buf](uint32_t v) {
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
  };
  buf.push_back(CssParser::CSS_CACHE_VERSION);
  push16(2);  // entryCount
  push16(1);  // styleCount
  push32(2);  // poolBytes ("ab")
  // entry 0 -> "b", entry 1 -> "a": descending, must be rejected
  push32(1);
  push16(0);
  push16(1);
  push32(0);
  push16(0);
  push16(1);
  buf.insert(buf.end(), kStyleWireBytes, 0);  // one default style record
  buf.push_back('a');
  buf.push_back('b');

  std::ofstream out(cacheFilePath(), std::ios::binary);
  out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
  out.close();

  CssParser parser(cachePath());
  EXPECT_FALSE(parser.loadFromCache());
  EXPECT_EQ(parser.ruleCount(), 0u);
}

TEST_F(CssParserTest, RuleCapDegradesGracefully) {
  CssParser parser(cachePath());
  std::string css;
  for (size_t i = 0; i < kMaxRules + 50; ++i) {
    css += ".c" + std::to_string(i) + " { font-weight: bold; }\n";
  }
  loadCss(parser, css);

  EXPECT_EQ(parser.ruleCount(), kMaxRules);
  EXPECT_EQ(parser.uniqueStyleCount(), 1u);
  // Rules accepted before the cap still resolve
  EXPECT_EQ(parser.resolveStyle("p", "c0").fontWeight, CssFontWeight::Bold);
}

TEST_F(CssParserTest, UniqueStyleCapDegradesGracefully) {
  CssParser parser(cachePath());
  std::string css;
  for (size_t i = 0; i < kMaxUniqueStyles + 40; ++i) {
    // Every body is distinct, so each rule needs a new pooled style
    css += ".u" + std::to_string(i) + " { text-indent: " + std::to_string(i + 1) + "px; }\n";
  }
  loadCss(parser, css);

  EXPECT_EQ(parser.uniqueStyleCount(), kMaxUniqueStyles);
  // Rules whose style could not be pooled were skipped, not corrupted
  EXPECT_EQ(parser.ruleCount(), kMaxUniqueStyles);
  const CssStyle first = parser.resolveStyle("p", "u0");
  EXPECT_TRUE(first.hasTextIndent());
  EXPECT_FLOAT_EQ(first.textIndent.value, 1.0f);
  const CssStyle overflow = parser.resolveStyle("p", "u" + std::to_string(kMaxUniqueStyles + 10));
  EXPECT_FALSE(overflow.hasTextIndent());
}

TEST_F(CssParserTest, PerChapterConvertedBookFullyResolves) {
  // Replica of issue #2591's book shape: one stylesheet per chapter, each
  // re-declaring the same handful of bodies under chapter-unique class names.
  // The old per-node map needed ~186KB for this shape and was truncated by a
  // heap guard on-device; the pooled store must hold every rule.
  constexpr int kFiles = 126;
  constexpr int kClassesPerFile = 10;
  static const char* kBodies[5] = {
      "font-weight : bolder ; text-indent : 1.5em ;", "font-style : italic ;",
      "text-align : justify ; text-indent : 1.5em ;", "margin-top : 2em ; margin-bottom : 2em ;",
      "text-align : center ; font-weight : normal ;",
  };

  CssParser parser(cachePath());
  for (int f = 0; f < kFiles; ++f) {
    std::string css;
    for (int c = 0; c < kClassesPerFile; ++c) {
      css += ".class-" + std::to_string(f) + "-" + std::to_string(c) + " { " + std::string(kBodies[c % 5]) + " }\n";
    }
    loadCss(parser, css);
  }

  EXPECT_EQ(parser.ruleCount(), static_cast<size_t>(kFiles * kClassesPerFile));
  EXPECT_EQ(parser.uniqueStyleCount(), 5u);

  // Every chapter's "bolder" class must resolve — including the last file,
  // which is exactly what got dropped on-device before the pooled store.
  for (int f = 0; f < kFiles; ++f) {
    const std::string boldClass = "class-" + std::to_string(f) + "-0";
    const CssStyle style = parser.resolveStyle("div", boldClass);
    EXPECT_EQ(style.fontWeight, CssFontWeight::Bold) << "chapter " << f << " lost its bold rule";
  }

  // And the whole set survives a cache round trip.
  ASSERT_TRUE(parser.saveToCache());
  CssParser reader(cachePath());
  ASSERT_TRUE(reader.loadFromCache());
  EXPECT_EQ(reader.ruleCount(), static_cast<size_t>(kFiles * kClassesPerFile));
  EXPECT_EQ(reader.resolveStyle("div", "class-125-0").fontWeight, CssFontWeight::Bold);
}

TEST_F(CssParserTest, ClearReleasesAllRules) {
  CssParser parser(cachePath());
  loadCss(parser, ".a { font-weight: bold; }\n");
  EXPECT_EQ(parser.ruleCount(), 1u);
  parser.clear();
  EXPECT_TRUE(parser.empty());
  EXPECT_EQ(parser.uniqueStyleCount(), 0u);
  EXPECT_FALSE(parser.resolveStyle("p", "a").hasFontWeight());
  // Parser stays usable after clear()
  loadCss(parser, ".b { font-style: italic; }\n");
  EXPECT_EQ(parser.resolveStyle("p", "b").fontStyle, CssFontStyle::Italic);
}

TEST_F(CssParserTest, InlineStyleParsingUnchanged) {
  const CssStyle style = CssParser::parseInlineStyle("font-weight: bolder; text-align: center");
  EXPECT_EQ(style.fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(style.textAlign, CssTextAlign::Center);
}

}  // namespace
