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

extern bool failNextTextBlockArena;

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

TEST_F(ChapterHtmlSlimParserTest, PreservesCurrentAnchorWhenFlushingStoredCellAnchors) {
  parser.tableRowStacked = true;
  parser.insideTableCell = true;
  parser.pendingAnchorId = "stored-anchor";
  parser.collectPendingTableAnchor();

  parser.pendingAnchorId = "current-anchor";
  parser.flushTableRowAnchorsForCell(0);

  ASSERT_EQ(parser.anchorData.size(), 1u);
  EXPECT_EQ(parser.anchorData.front().first, "stored-anchor");
  EXPECT_EQ(parser.pendingAnchorId, "current-anchor");
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

TEST_F(ChapterHtmlSlimParserTest, DoesNotEmitEmptyPageForOversizedLine) {
  parser.viewportHeight = 8;
  parser.currentPage = std::make_unique<Page>();
  size_t completed = 0;
  parser.completePageFn = [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) {
    ++completed;
    EXPECT_FALSE(page->elements.empty());
  };
  auto line =
      std::make_shared<TextBlock>(std::vector<std::string>{}, std::vector<int16_t>{},
                                  std::vector<EpdFontFamily::Style>{}, std::vector<uint8_t>{}, std::vector<uint16_t>{});
  parser.addLineToPage(line, 0);
  EXPECT_EQ(completed, 0u);
  ASSERT_NE(parser.currentPage, nullptr);
  EXPECT_EQ(parser.currentPage->elements.size(), 1u);
}

TEST_F(ChapterHtmlSlimParserTest, MapsCellAliasesAfterItsTocPageBreak) {
  parser.tableRowStacked = true;
  parser.insideTableCell = true;
  parser.tocAnchors = {"chapter"};
  parser.currentPage = std::make_unique<Page>();
  parser.currentPage->elements.push_back(std::make_shared<PageHorizontalRule>(100, 1, 0, 0));
  parser.completePageFn = [](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t) {};
  parser.pendingAnchorId = "alias";
  parser.collectPendingTableAnchor();
  parser.pendingAnchorId = "chapter";
  parser.collectPendingTableAnchor();
  parser.pendingAnchorId = "next-cell";
  parser.flushPendingTableCellAnchors();
  ASSERT_EQ(parser.anchorData.size(), 2u);
  for (const auto& anchor : parser.anchorData) EXPECT_EQ(anchor.second, 1u);
  EXPECT_EQ(parser.pendingAnchorId, "next-cell");
}

TEST_F(ChapterHtmlSlimParserTest, DoesNotConsumeTextWhenItsArenaAllocationFails) {
  parser.currentTextBlock->addWord("preserved", EpdFontFamily::REGULAR);
  bool emitted = false;
  failNextTextBlockArena = true;
  EXPECT_FALSE(parser.currentTextBlock->layoutAndExtractLines(
      renderer, 0, 100, [&](std::shared_ptr<TextBlock>, uint32_t) { emitted = true; }));
  EXPECT_FALSE(emitted);
  EXPECT_EQ(parser.currentTextBlock->size(), 1u);
}

TEST_F(ChapterHtmlSlimParserTest, RejectsSectionAfterGridCellArenaFailure) {
  parser.tableRowCells.reserve(2);
  for (int i = 0; i < 2; ++i) {
    auto cell = std::make_unique<ParsedText>(false);
    cell->addWord("cell", EpdFontFamily::REGULAR);
    parser.tableRowCells.push_back(std::move(cell));
  }
  failNextTextBlockArena = true;
  parser.finishTableRow();
  EXPECT_TRUE(parser.layoutFailed);
  EXPECT_EQ(parser.parseStep(), ChapterHtmlSlimParser::ParseStatus::Error);
  EXPECT_FALSE(parser.finishParse());
}

TEST_F(ChapterHtmlSlimParserTest, RejectsSectionAfterStackedCellArenaFailure) {
  parser.tableRowStacked = true;
  parser.insideTableCell = true;
  parser.currentTextBlock->addWord("cell", EpdFontFamily::REGULAR);
  failNextTextBlockArena = true;
  parser.closeTableCell();
  EXPECT_TRUE(parser.layoutFailed);
  EXPECT_FALSE(parser.finishParse());
}

INSTANTIATE_TEST_SUITE_P(CssVerticalAlign, ChapterHtmlSlimParserTest,
                         ::testing::Values("vertical-align: super", "vertical-align: sub"));

TEST_F(ChapterHtmlSlimParserTest, ParagraphWithHiddenAttributeShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "p", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
}

TEST_F(ChapterHtmlSlimParserTest, HeaderWithHiddenAttributeShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "h1", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
}

TEST_F(ChapterHtmlSlimParserTest, SpanWithHiddenAttributeShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Before ", 7);
  ChapterHtmlSlimParser::startElement(&parser, "span", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ChapterHtmlSlimParser::characterData(&parser, " After ", 7);

  ASSERT_EQ(parser.currentTextBlock->size(), 2);
  ASSERT_EQ(parser.currentTextBlock->words[0], "Before");
  ASSERT_EQ(parser.currentTextBlock->words[1], "After");
}

TEST_F(ChapterHtmlSlimParserTest, DivWithHiddenAttributeContentShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "div", attributes);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
}

}  // namespace
