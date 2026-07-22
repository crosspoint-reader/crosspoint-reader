// Tests for ChapterHtmlSlimParser's HTML->layout pipeline, run entirely on
// the host. See test/chapter_html_slim_parser/stubs/ for the HAL/Arduino
// doubles and TestFont.h for the deterministic fixed-width test font; the
// parser, CSS resolver, word layout, and text-block packing under test are
// all the real production code from lib/Epub.
//
// GitHub issue #291: <li> items in an <ol> should be numbered ("1.", "2.",
// ...) instead of always getting the unordered-list bullet, and
// list-style-type: none should suppress the marker entirely.

#include "Epub/parsers/ChapterHtmlSlimParser.h"

#include <gtest/gtest.h>

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Epub/Page.h"
#include "Epub/blocks/TextBlock.h"
#include "TestFont.h"

namespace {

constexpr int kFontId = 1;
constexpr uint16_t kViewportWidth = 600;
constexpr uint16_t kViewportHeight = 4000;

std::string writeFixture(const std::string& html) {
  static int counter = 0;
  const auto path = std::filesystem::temp_directory_path() /
                     ("chapter_html_slim_parser_test_" + std::to_string(counter++) + ".html");
  std::ofstream out(path, std::ios::binary);
  out << html;
  out.close();
  return path.string();
}

using Line = std::vector<std::pair<std::string, EpdFontFamily::Style>>;

std::vector<Line> parseHtmlIntoLines(const std::string& html) {
  HalDisplay halDisplay;
  GfxRenderer renderer(halDisplay);
  renderer.insertFont(kFontId, testfont::makeTestFontFamily());

  const std::string filepath = writeFixture(html);

  std::vector<std::unique_ptr<Page>> pages;
  const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)> completePageFn =
      [&pages](std::unique_ptr<Page> page, uint16_t, uint16_t) { pages.push_back(std::move(page)); };

  ChapterHtmlSlimParser parser(/*epub=*/nullptr, filepath, renderer, kFontId, /*lineCompression=*/1.0f,
                               /*extraParagraphSpacing=*/false, static_cast<uint8_t>(CssTextAlign::Left),
                               kViewportWidth, kViewportHeight, /*hyphenationEnabled=*/false,
                               /*focusReadingEnabled=*/false, completePageFn, /*embeddedStyle=*/false,
                               /*contentBase=*/"", /*imageBasePath=*/"");

  const bool ok = parser.parseAndBuildPages();
  std::filesystem::remove(filepath);
  if (!ok) return {};

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
  const auto lines =
      parseHtmlIntoLines("<html><body><ol><li>Apple</li><li>Banana</li></ol></body></html>");

  ASSERT_EQ(lines.size(), 2u);
  ASSERT_FALSE(lines[0].empty());
  EXPECT_EQ(lines[0][0].first, "1.");
  ASSERT_FALSE(lines[1].empty());
  EXPECT_EQ(lines[1][0].first, "2.");
}

TEST(ChapterHtmlSlimParserListTest, ListStyleTypeNoneSuppressesMarker) {
  const auto lines = parseHtmlIntoLines(
      "<html><body><ul style=\"list-style-type: none\"><li>Apple</li></ul></body></html>");

  ASSERT_EQ(lines.size(), 1u);
  ASSERT_FALSE(lines[0].empty());
  EXPECT_EQ(lines[0][0].first, "Apple");
}

TEST(ChapterHtmlSlimParserListTest, OrderedListStyleTypeNoneSuppressesNumber) {
  const auto lines = parseHtmlIntoLines(
      "<html><body><ol style=\"list-style-type: none\"><li>Apple</li></ol></body></html>");

  ASSERT_EQ(lines.size(), 1u);
  ASSERT_FALSE(lines[0].empty());
  EXPECT_EQ(lines[0][0].first, "Apple");
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
  const auto lines =
      parseHtmlIntoLines("<html><body><ul><li><p>First</p><p>Second</p></li></ul></body></html>");

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

