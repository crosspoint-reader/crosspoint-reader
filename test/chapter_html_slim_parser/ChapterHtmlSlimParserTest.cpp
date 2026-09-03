#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

namespace {

class ChapterHtmlSlimParserTest : public ::testing::TestWithParam<const char*> {
 protected:
  std::string filepath = "unused.xhtml";
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  ChapterHtmlSlimParser parser{nullptr,
                               filepath,
                               renderer,
                               0,
                               1.0f,
                               false,
                               0,
                               static_cast<uint16_t>(renderer.getScreenWidth()),
                               static_cast<uint16_t>(renderer.getScreenHeight()),
                               false,
                               false,
                               {},
                               true,
                               "",
                               "",
                               0,
                               {},
                               nullptr,
                               &cssParser};

  void SetUp() override { parser.currentTextBlock = std::make_unique<ParsedText>(false); }
};

TEST_P(ChapterHtmlSlimParserTest, KeepsCssVerticalAlignAndInternalLinkMetadata) {
  const char* verticalAlign = GetParam();
  const char* expectedHref = "#note-target";
  const XML_Char* attributes[] = {"href", expectedHref, "style", verticalAlign, nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "a", attributes);
  const uint8_t linkId = parser.currentFootnoteLinkId;
  ASSERT_NE(linkId, 0u);
  ChapterHtmlSlimParser::characterData(&parser, "1", 1);
  ChapterHtmlSlimParser::endElement(&parser, "a");

  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  const auto style = parser.currentTextBlock->getWordStyleAt(0);
  const auto expectedStyle =
      std::string(verticalAlign).find("super") != std::string::npos ? EpdFontFamily::SUP : EpdFontFamily::SUB;
  EXPECT_NE(static_cast<uint8_t>(style) & static_cast<uint8_t>(expectedStyle), 0u);

  ASSERT_EQ(parser.pendingFootnotes.size(), 1u);
  const FootnoteEntry& footnote = parser.pendingFootnotes.front().second;
  EXPECT_STREQ(footnote.href, expectedHref);
  ASSERT_EQ(parser.currentTextBlock->wordLinkIds.size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->wordLinkIds.front(), linkId);
  EXPECT_TRUE(parser.currentTextBlock->linkTargetMatches(linkId, expectedHref));
}

TEST_F(ChapterHtmlSlimParserTest, KeepsNestedTableAnchorsDeferredForTheirOuterCell) {
  parser.tableDepth = 2;
  parser.insideTableCell = true;
  parser.pendingAnchorId = "first-anchor";
  parser.completedPageCount = 7;
  const XML_Char* attributes[] = {"id", "second-anchor", nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "aside", attributes);

  EXPECT_TRUE(parser.anchorData.empty());
  EXPECT_EQ(parser.tableRowAnchorCount, 1u);
  EXPECT_EQ(static_cast<uint8_t>(parser.tableRowAnchorStorage[0]), 0u);
  EXPECT_STREQ(parser.tableRowAnchorStorage.data() + 1, "first-anchor");
  EXPECT_EQ(parser.pendingAnchorId, "second-anchor");
}

TEST_F(ChapterHtmlSlimParserTest, ReclaimsFlushedTableAnchorStorageBeforeCollectingAnother) {
  parser.tableRowStacked = true;
  parser.insideTableCell = true;
  parser.pendingAnchorId.assign(ChapterHtmlSlimParser::MAX_GRID_TABLE_ANCHOR_BYTES - 2, 'a');
  parser.collectPendingTableAnchor();
  ASSERT_EQ(parser.tableRowAnchorBytes, ChapterHtmlSlimParser::MAX_GRID_TABLE_ANCHOR_BYTES);

  parser.flushTableRowAnchorsForCell(0);
  ASSERT_EQ(parser.anchorData.size(), 1u);
  parser.pendingAnchorId = "next-anchor";
  parser.collectPendingTableAnchor();

  EXPECT_EQ(parser.anchorData.size(), 1u);
  EXPECT_EQ(parser.tableRowAnchorCount, 1u);
  EXPECT_EQ(static_cast<uint8_t>(parser.tableRowAnchorStorage[0]), 0u);
  EXPECT_STREQ(parser.tableRowAnchorStorage.data() + 1, "next-anchor");
  EXPECT_TRUE(parser.pendingAnchorId.empty());
}

INSTANTIATE_TEST_SUITE_P(CssVerticalAlign, ChapterHtmlSlimParserTest,
                         ::testing::Values("vertical-align: super", "vertical-align: sub"));

}  // namespace
