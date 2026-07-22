// Tests for ChapterHtmlSlimParser's HTML->layout pipeline, run entirely on
// the host. See test/chapter_html_slim_parser/stubs/ for the HAL/Arduino
// doubles and TestFont.h for the deterministic fixed-width test font; the
// parser, CSS resolver, word layout, and text-block packing under test are
// all the real production code from lib/Epub.
//
// GitHub issue #291: <li> items in an <ol> should be numbered ("1.", "2.",
// ...) instead of always getting the unordered-list bullet, and
// list-style-type: none should suppress the marker entirely.

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "Epub/Page.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/css/CssParser.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#include "TestFont.h"

namespace {

constexpr int kFontId = 1;
constexpr uint16_t kViewportWidth = 600;
constexpr uint16_t kViewportHeight = 4000;

std::string writeFixture(const std::string& html) {
  // Random (not just an incrementing counter) so concurrent CTest processes running
  // this same binary under `ctest -j` (one process per TEST(), one gtest_filter each)
  // never race on the same path in the shared OS temp directory. A per-process counter
  // alone collides: every process starts back at 0, so parallel tests can read a
  // sibling test's fixture mid-write and see garbage HTML.
  static std::mt19937_64 rng(std::random_device{}());
  const auto path =
      std::filesystem::temp_directory_path() / ("chapter_html_slim_parser_test_" + std::to_string(rng()) + ".html");
  std::ofstream out(path, std::ios::binary);
  out << html;
  out.close();
  return path.string();
}

std::string writeCssFixture(const std::string& css) {
  static std::mt19937_64 rng(std::random_device{}());
  const auto path =
      std::filesystem::temp_directory_path() / ("chapter_html_slim_parser_test_" + std::to_string(rng()) + ".css");
  std::ofstream out(path, std::ios::binary);
  out << css;
  out.close();
  return path.string();
}

using Line = std::vector<std::pair<std::string, EpdFontFamily::Style>>;

// Runs the real parser end-to-end and returns the resulting pages. Shared by
// parseHtmlIntoLines() (word/style assertions) and any test that also needs
// to inspect a line's resolved BlockStyle (e.g. hanging-indent geometry).
std::vector<std::unique_ptr<Page>> parseHtmlIntoPages(const std::string& html, const std::string& css = "") {
  HalDisplay halDisplay;
  GfxRenderer renderer(halDisplay);
  renderer.insertFont(kFontId, testfont::makeTestFontFamily());

  const std::string filepath = writeFixture(html);

  std::vector<std::unique_ptr<Page>> pages;
  const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)> completePageFn =
      [&pages](std::unique_ptr<Page> page, uint16_t, uint16_t) { pages.push_back(std::move(page)); };

  // A real CssParser is required for inline style="..." attributes and class-based
  // CSS resolution to work at all: ChapterHtmlSlimParser only calls
  // CssParser::parseInlineStyle / resolveStyle when self->cssParser is non-null.
  // When css is provided, load it as a stylesheet so class selectors resolve.
  CssParser cssParser("");
  if (!css.empty()) {
    const std::string cssPath = writeCssFixture(css);
    HalFile cssFile;
    if (Storage.openFileForRead("TST", cssPath, cssFile)) {
      cssParser.loadFromStream(cssFile);
    }
    std::filesystem::remove(cssPath);
  }

  ChapterHtmlSlimParser parser(/*epub=*/nullptr, filepath, renderer, kFontId, /*lineCompression=*/1.0f,
                               /*extraParagraphSpacing=*/false, static_cast<uint8_t>(CssTextAlign::Left),
                               kViewportWidth, kViewportHeight, /*hyphenationEnabled=*/false,
                               /*focusReadingEnabled=*/false, completePageFn, /*embeddedStyle=*/true,
                               /*contentBase=*/"", /*imageBasePath=*/"", /*imageRendering=*/0,
                               /*tocAnchors=*/{}, /*popupFn=*/nullptr, &cssParser);

  const bool ok = parser.parseAndBuildPages();
  std::filesystem::remove(filepath);
  if (!ok) return {};
  return pages;
}

std::vector<Line> parseHtmlIntoLines(const std::string& html, const std::string& css = "") {
  const auto pages = parseHtmlIntoPages(html, css);

  std::vector<Line> lines;
  for (const auto& page : pages) {
    for (const auto& element : page->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto* pageLine = static_cast<const PageLine*>(element.get());
      const auto& block = pageLine->getBlock();
      Line line;
      for (uint16_t i = 0; i < block->wordCount(); i++) {
        line.emplace_back(block->wordText(i), block->wordStyle(i));
      }
      lines.push_back(std::move(line));
    }
  }
  return lines;
}

// Returns the BlockStyle of every PageLine, in order, across all pages.
std::vector<BlockStyle> parseHtmlIntoBlockStyles(const std::string& html, const std::string& css = "") {
  const auto pages = parseHtmlIntoPages(html, css);

  std::vector<BlockStyle> styles;
  for (const auto& page : pages) {
    for (const auto& element : page->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto* pageLine = static_cast<const PageLine*>(element.get());
      styles.push_back(pageLine->getBlock()->getBlockStyle());
    }
  }
  return styles;
}

constexpr const char* kBullet = "\xE2\x80\xA2";

}  // namespace

TEST(ChapterHtmlSlimParserListTest, UnorderedListItemGetsBulletPrefix) {
  const auto lines = parseHtmlIntoLines("<html><body><ul><li>Apple</li></ul></body></html>");

  ASSERT_EQ(lines.size(), 1u);
  ASSERT_GE(lines[0].size(), 2u);
  EXPECT_EQ(lines[0][0].first, kBullet);
  EXPECT_EQ(lines[0][1].first, "Apple");
}

TEST(ChapterHtmlSlimParserListTest, OrderedListItemsGetNumberedPrefix) {
  const auto lines = parseHtmlIntoLines("<html><body><ol><li>Apple</li><li>Banana</li></ol></body></html>");

  ASSERT_EQ(lines.size(), 2u);
  ASSERT_FALSE(lines[0].empty());
  EXPECT_EQ(lines[0][0].first, "1.");
  ASSERT_FALSE(lines[1].empty());
  EXPECT_EQ(lines[1][0].first, "2.");
}

TEST(ChapterHtmlSlimParserListTest, NestedOrderedListsRestartAndResumeCounters) {
  const auto lines = parseHtmlIntoLines(
      "<html><body><ol><li>Outer one"
      "<ol><li>Inner one</li></ol>"
      "</li><li>Outer two</li></ol></body></html>");

  ASSERT_EQ(lines.size(), 3u);
  ASSERT_FALSE(lines[0].empty());
  EXPECT_EQ(lines[0][0].first, "1.");
  ASSERT_FALSE(lines[1].empty());
  EXPECT_EQ(lines[1][0].first, "1.");
  ASSERT_FALSE(lines[2].empty());
  EXPECT_EQ(lines[2][0].first, "2.");
}

// Regression test: a display:none nested <ul>/<ol> returns from startElement
// before pushing a ListContext (see the hasDisplay()/CssDisplay::None fast
// path), so its closing tag must not unconditionally pop the *outer* list's
// context -- otherwise later outer <li>s lose their number/bullet entirely.
TEST(ChapterHtmlSlimParserListTest, HiddenNestedListDoesNotPopOuterListContext) {
  const auto lines = parseHtmlIntoLines(
      "<html><body><ol><li>Outer one</li>"
      "<ol style=\"display: none\"><li>Hidden</li></ol>"
      "<li>Outer two</li></ol></body></html>");

  ASSERT_EQ(lines.size(), 2u);
  ASSERT_FALSE(lines[0].empty());
  EXPECT_EQ(lines[0][0].first, "1.");
  ASSERT_FALSE(lines[1].empty());
  EXPECT_EQ(lines[1][0].first, "2.");
}

TEST(ChapterHtmlSlimParserListTest, ListStyleTypeNoneSuppressesMarker) {
  const auto lines =
      parseHtmlIntoLines("<html><body><ul style=\"list-style-type: none\"><li>Apple</li></ul></body></html>");

  ASSERT_EQ(lines.size(), 1u);
  ASSERT_FALSE(lines[0].empty());
  EXPECT_EQ(lines[0][0].first, "Apple");
}

TEST(ChapterHtmlSlimParserListTest, OrderedListStyleTypeNoneSuppressesNumber) {
  const auto lines =
      parseHtmlIntoLines("<html><body><ol style=\"list-style-type: none\"><li>Apple</li></ol></body></html>");

  ASSERT_EQ(lines.size(), 1u);
  ASSERT_FALSE(lines[0].empty());
  EXPECT_EQ(lines[0][0].first, "Apple");
}

TEST(ChapterHtmlSlimParserListTest, ExplicitListStyleTypeDiscStillShowsBullet) {
  const auto lines =
      parseHtmlIntoLines("<html><body><ul style=\"list-style-type: disc\"><li>Apple</li></ul></body></html>");

  ASSERT_EQ(lines.size(), 1u);
  ASSERT_GE(lines[0].size(), 2u);
  EXPECT_EQ(lines[0][0].first, kBullet);
  EXPECT_EQ(lines[0][1].first, "Apple");
}

TEST(ChapterHtmlSlimParserListTest, UnrecognizedListStyleTypeFallsBackToBullet) {
  const auto lines =
      parseHtmlIntoLines("<html><body><ul style=\"list-style-type: circle\"><li>Apple</li></ul></body></html>");

  ASSERT_EQ(lines.size(), 1u);
  ASSERT_GE(lines[0].size(), 2u);
  EXPECT_EQ(lines[0][0].first, kBullet);
  EXPECT_EQ(lines[0][1].first, "Apple");
}

TEST(ChapterHtmlSlimParserListTest, ExplicitListStyleTypeDecimalStillShowsNumbers) {
  const auto lines = parseHtmlIntoLines(
      "<html><body><ol style=\"list-style-type: decimal\"><li>Apple</li><li>Banana"
      "</li></ol></body></html>");

  ASSERT_EQ(lines.size(), 2u);
  ASSERT_FALSE(lines[0].empty());
  EXPECT_EQ(lines[0][0].first, "1.");
  ASSERT_FALSE(lines[1].empty());
  EXPECT_EQ(lines[1][0].first, "2.");
}

TEST(ChapterHtmlSlimParserListTest, ClassBasedListStyleTypeNoneSuppressesBullet) {
  const auto lines = parseHtmlIntoLines(
      "<html><body><ul class=\"list-simple1\">"
      "<li><p class=\"list-item1\">Apple</p></li>"
      "<li><p class=\"list-item1\">Banana</p></li>"
      "</ul></body></html>",
      ".list-simple1 { list-style-type: none; margin-left: 1em; }"
      ".list-item1 { text-indent: 0; }");

  ASSERT_EQ(lines.size(), 2u);
  ASSERT_FALSE(lines[0].empty());
  EXPECT_EQ(lines[0][0].first, "Apple");
  ASSERT_FALSE(lines[1].empty());
  EXPECT_EQ(lines[1][0].first, "Banana");
}

TEST(ChapterHtmlSlimParserListTest, ClassBasedListStyleTypeNoneOnOrderedListSuppressesNumbers) {
  const auto lines = parseHtmlIntoLines(
      "<html><body><ol class=\"no-num\">"
      "<li>Apple</li><li>Banana</li>"
      "</ol></body></html>",
      ".no-num { list-style-type: none; }");

  ASSERT_EQ(lines.size(), 2u);
  ASSERT_FALSE(lines[0].empty());
  EXPECT_EQ(lines[0][0].first, "Apple");
  ASSERT_FALSE(lines[1].empty());
  EXPECT_EQ(lines[1][0].first, "Banana");
}

// Regression test for a real-world EPUB CSS pattern: a <ul> container with its
// own margin-left/padding-left (dropped prior to a fix that made <ul>/<ol>
// full block containers), whose <li><p> children rely on that container inset
// to counterbalance a hanging text-indent. Without the container's
// contribution, leftInset()+textIndent goes negative and the first line's
// glyphs render off the left edge of the page, with continuation lines
// sitting almost flush against the margin instead of hanging-indented.
TEST(ChapterHtmlSlimParserListTest, ListContainerMarginCounterbalancesHangingIndent) {
  const auto styles = parseHtmlIntoBlockStyles(
      "<html><body>"
      "<ul class=\"list-simple1\">"
      "<li><p class=\"list-item1\">Sketching User Experiences by Bill Buxton.</p></li>"
      "<li><p class=\"list-item1\">Have Paper, Will Prototype by Bill Lucas.</p></li>"
      "</ul>"
      "</body></html>",
      ".list-simple1 { margin-top: 1em; padding-left: 1.4em; margin-left: 0.1em; margin-bottom: 1em; "
      "margin-right: 0.1em; text-align: left; list-style-type: none; }"
      ".list-item1 { margin-top: 0.1em; margin-bottom: 0.1em; margin-right: 0em; margin-left: 0.1em; "
      "text-indent: -1.5em; }");

  ASSERT_FALSE(styles.empty());
  const auto& firstLine = styles.front();
  // The <ul>'s own padding-left (1.4em) + margin-left (0.1em) must still be
  // present on the <li>/<p> block instead of being silently dropped.
  EXPECT_GT(firstLine.leftInset(), 0);
  EXPECT_TRUE(firstLine.textIndentDefined);
  EXPECT_LT(firstLine.textIndent, 0);
  // The actual regression: the hanging-indent's negative text-indent must not
  // push the first line's start position past the left edge of the page.
  EXPECT_GE(firstLine.leftInset() + firstLine.textIndent, 0)
      << "leftInset=" << firstLine.leftInset() << " textIndent=" << firstLine.textIndent
      << " -- first line would render off the left edge";
}

// Regression tests for PR #2589 (GitHub issue #956): when <li> wraps a
// block-level child like <p>, the bullet must stay inline with the child's
// text instead of being flushed to its own line above it.

TEST(ChapterHtmlSlimParserListItemBlockTest, BulletStaysInlineWithNestedParagraphText) {
  const auto lines = parseHtmlIntoLines("<html><body><ul><li><p>Apple</p></li></ul></body></html>");

  ASSERT_EQ(lines.size(), 1u);
  ASSERT_EQ(lines[0].size(), 2u);
  EXPECT_EQ(lines[0][0].first, kBullet);
  EXPECT_EQ(lines[0][1].first, "Apple");
}

TEST(ChapterHtmlSlimParserListItemBlockTest, SecondParagraphInSameListItemStartsNewLineWithoutBullet) {
  const auto lines = parseHtmlIntoLines("<html><body><ul><li><p>First</p><p>Second</p></li></ul></body></html>");

  ASSERT_EQ(lines.size(), 2u);
  ASSERT_EQ(lines[0].size(), 2u);
  EXPECT_EQ(lines[0][0].first, kBullet);
  EXPECT_EQ(lines[0][1].first, "First");
  ASSERT_EQ(lines[1].size(), 1u);
  EXPECT_EQ(lines[1][0].first, "Second");
}

TEST(ChapterHtmlSlimParserListItemBlockTest, NestedListInsideListItemKeepsEachBulletInline) {
  const auto lines = parseHtmlIntoLines(
      "<html><body><ul><li><p>Outer</p>"
      "<ul><li><p>Inner</p></li></ul>"
      "</li></ul></body></html>");

  ASSERT_EQ(lines.size(), 2u);
  ASSERT_EQ(lines[0].size(), 2u);
  EXPECT_EQ(lines[0][0].first, kBullet);
  EXPECT_EQ(lines[0][1].first, "Outer");
  ASSERT_EQ(lines[1].size(), 2u);
  EXPECT_EQ(lines[1][0].first, kBullet);
  EXPECT_EQ(lines[1][1].first, "Inner");
}

TEST(ChapterHtmlSlimParserListItemBlockTest, EmptyListItemDoesNotAbsorbNextItemsBullet) {
  const auto lines = parseHtmlIntoLines("<html><body><ul><li></li><li>Text</li></ul></body></html>");

  ASSERT_EQ(lines.size(), 2u);
  ASSERT_EQ(lines[0].size(), 1u);
  EXPECT_EQ(lines[0][0].first, kBullet);
  ASSERT_EQ(lines[1].size(), 2u);
  EXPECT_EQ(lines[1][0].first, kBullet);
  EXPECT_EQ(lines[1][1].first, "Text");
}
