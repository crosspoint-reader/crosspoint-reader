#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

static thread_local bool failNextArrayAllocation = false;
static thread_local size_t arrayAllocationsToSkip = 0;
static thread_local size_t additionalArrayFailures = 0;
static thread_local size_t allocationSizeToFail = 0;
static thread_local size_t matchingAllocationsToSkip = 0;
void* operator new(size_t size, const std::nothrow_t&) noexcept {
  if (allocationSizeToFail == size) {
    if (matchingAllocationsToSkip > 0) {
      --matchingAllocationsToSkip;
    } else {
      allocationSizeToFail = 0;
      return nullptr;
    }
  }
  return ::operator new(size);
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  if (failNextArrayAllocation) {
    if (arrayAllocationsToSkip > 0) {
      --arrayAllocationsToSkip;
    } else {
      if (additionalArrayFailures > 0) {
        --additionalArrayFailures;
      } else {
        failNextArrayAllocation = false;
      }
      return nullptr;
    }
  }
  return ::operator new[](size);
}

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

  void TearDown() override {
    EXPECT_EQ(allocationSizeToFail, 0u);
    EXPECT_EQ(matchingAllocationsToSkip, 0u);
    EXPECT_FALSE(failNextArrayAllocation);
    EXPECT_EQ(arrayAllocationsToSkip, 0u);
    EXPECT_EQ(additionalArrayFailures, 0u);
    additionalArrayFailures = 0;
    arrayAllocationsToSkip = 0;
    allocationSizeToFail = 0;
    matchingAllocationsToSkip = 0;
    failNextArrayAllocation = false;
  }
};

TEST_F(ChapterHtmlSlimParserTest, RubySurvivesPartialParagraphExtraction) {
  ParsedText text(false);
  text.addWord("a", EpdFontFamily::REGULAR);
  text.addWord("b", EpdFontFamily::REGULAR);
  text.addWord("c", EpdFontFamily::REGULAR);
  text.setRubyForWordAt(2, "c");
  size_t lines = 0;
  text.layoutAndExtractLines(
      renderer, 0, 20,
      [&](std::unique_ptr<TextBlock> line, auto) {
        ++lines;
        EXPECT_TRUE(line->getRubyTexts().empty());
      },
      false);
  EXPECT_EQ(lines, 1u);
  const size_t retainedWords = text.size();
  ASSERT_GT(retainedWords, 0u);
  ASSERT_LT(retainedWords, 3u);
  text.layoutAndExtractLines(renderer, 0, 200, [&](std::unique_ptr<TextBlock> line, auto) {
    ++lines;
    ASSERT_EQ(line->getRubyTexts().size(), retainedWords);
    EXPECT_EQ(line->getRubyTexts().back(), "c");
    for (size_t i = 0; i + 1 < retainedWords; ++i) EXPECT_TRUE(line->getRubyTexts()[i].empty());
  });
  EXPECT_EQ(lines, 2u);
}

TEST_F(ChapterHtmlSlimParserTest, UnequalTableCellsAndRubySurvivePageBreaks) {
  parser.viewportWidth = 240;
  parser.viewportHeight = 32;
  ASSERT_TRUE(parser.tableRowCells.reserve(2));
  std::multiset<std::string> expected;
  for (int column = 0; column < 2; ++column) {
    auto cell = std::make_unique<ParsedText>(false);
    for (int index = 0; index < (column == 0 ? 30 : 3); ++index) {
      const auto word = std::string(column == 0 ? "left" : "right") + std::to_string(index);
      expected.insert(word);
      cell->addWord(word, EpdFontFamily::REGULAR);
    }
    if (column == 0) cell->setRubyGroupAt(0, 2, "reading");
    ASSERT_TRUE(parser.tableRowCells.push_back(std::move(cell)));
  }
  std::multiset<std::string> actual;
  unsigned pages = 0;
  unsigned rubyLines = 0;
  auto inspect = [&](std::unique_ptr<Page> page, auto, auto, auto) {
    ++pages;
    for (const auto& element : page->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      const auto& block = *line.getBlock();
      ASSERT_TRUE(block.valid());
      EXPECT_LE(element->yPos + 16 + block.getRubyShift(12), parser.viewportHeight);
      rubyLines += block.hasRuby();
      for (uint16_t word = 0; word < block.wordCount(); ++word) actual.insert(block.wordText(word));
    }
  };
  parser.completePageFn = inspect;
  parser.finishTableRow();
  ASSERT_NE(parser.currentPage, nullptr);
  inspect(std::move(parser.currentPage), 0, 0, 0);
  EXPECT_GT(pages, 2u);
  EXPECT_EQ(rubyLines, 1u);
  EXPECT_EQ(actual, expected);
  for (const auto& lines : parser.tableCellLines) EXPECT_TRUE(lines.empty());
}

TEST_F(ChapterHtmlSlimParserTest, GridCellsPreserveTextSpacing) {
  parser.viewportWidth = 240;
  parser.setTextSpacing(-1, 150);
  ASSERT_TRUE(parser.tableRowCells.reserve(2));
  for (int column = 0; column < 2; ++column) {
    BlockStyle style;
    style.alignment = CssTextAlign::Left;
    style.textIndentDefined = true;
    auto cell = std::make_unique<ParsedText>(false, false, false, style);
    cell->addWord("ab", EpdFontFamily::REGULAR);
    cell->addWord("cd", EpdFontFamily::REGULAR);
    ASSERT_TRUE(parser.tableRowCells.push_back(std::move(cell)));
  }
  parser.finishTableRow();
  ASSERT_FALSE(parser.layoutFailed);
  ASSERT_NE(parser.currentPage, nullptr);
  unsigned lines = 0;
  for (const auto& element : parser.currentPage->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& block = *static_cast<const PageLine&>(*element).getBlock();
    ++lines;
    ASSERT_EQ(block.wordCount(), 2);
    EXPECT_EQ(block.getBlockStyle().characterSpacing, -1);
    EXPECT_EQ(block.wordXpos(1) - block.wordXpos(0), 21);  // 15 px word plus 150% of a 4 px space.
  }
  EXPECT_EQ(lines, 2u);
}

TEST_F(ChapterHtmlSlimParserTest, PageImageDeserializeRejectsMissingImageBlock) {
  const auto path = std::filesystem::temp_directory_path() / "crosspoint-missing-image-cache.bin";
  {
    HalFile output;
    ASSERT_TRUE(output.open(path.c_str(), "wb"));
    const int16_t coordinates[] = {0, 0};
    output.write(coordinates, sizeof(coordinates));
  }
  HalFile input;
  ASSERT_TRUE(input.open(path.c_str(), "rb"));
  EXPECT_EQ(PageImage::deserialize(input), nullptr);
}

TEST_F(ChapterHtmlSlimParserTest, PageGridDeserializeRejectsAllocationFailure) {
  const auto path = std::filesystem::temp_directory_path() / "crosspoint-grid-allocation-cache.bin";
  {
    HalFile output;
    ASSERT_TRUE(output.open(path.c_str(), "wb"));
    PageTableGridRow grid(240, 20, 2, 0, 0);
    ASSERT_TRUE(grid.serialize(output));
  }
  {
    HalFile input;
    ASSERT_TRUE(input.open(path.c_str(), "rb"));
    allocationSizeToFail = sizeof(PageTableGridRow);
    EXPECT_EQ(PageTableGridRow::deserialize(input), nullptr);
  }
  std::filesystem::remove(path);
}

TEST_F(ChapterHtmlSlimParserTest, PageElementReserveIsNotASerializedCountLimit) {
  const auto path = std::filesystem::temp_directory_path() / "crosspoint-page-reserve-cache.bin";
  constexpr size_t count = 257;
  {
    Page page;
    ASSERT_TRUE(page.elements.reserve(count));
    for (size_t i = 0; i < count; ++i) {
      ASSERT_TRUE(
          page.elements.push_back(std::make_unique<PageTableGridRow>(240, 2, 2, 0, static_cast<int16_t>(i * 2))));
    }
    HalFile output;
    ASSERT_TRUE(output.open(path.c_str(), "wb"));
    ASSERT_TRUE(page.serialize(output));
  }
  {
    HalFile input;
    ASSERT_TRUE(input.open(path.c_str(), "rb"));
    auto page = Page::deserialize(input);
    ASSERT_NE(page, nullptr);
    ASSERT_EQ(page->elements.size(), count);
    for (size_t i = 0; i < count; ++i) {
      EXPECT_EQ(page->elements[i]->getTag(), TAG_PageTableGridRow);
      EXPECT_EQ(page->elements[i]->yPos, static_cast<int16_t>(i * 2));
    }
  }
  std::filesystem::remove(path);
}

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

  const std::string currentAnchor(ChapterHtmlSlimParser::MAX_GRID_TABLE_ANCHOR_BYTES + 1, 'x');
  parser.pendingAnchorId = currentAnchor;
  const char* pendingStorage = parser.pendingAnchorId.data();
  parser.flushTableRowAnchorsForCell(0);

  ASSERT_EQ(parser.anchorData.size(), 1u);
  EXPECT_STREQ(parser.anchorData[0].id.get(), "stored-anchor");
  EXPECT_EQ(parser.pendingAnchorId, currentAnchor);
  EXPECT_EQ(parser.pendingAnchorId.data(), pendingStorage);
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

TEST_F(ChapterHtmlSlimParserTest, OwnsAnchorIdsLongerThanRowStorage) {
  std::string id(ChapterHtmlSlimParser::MAX_GRID_TABLE_ANCHOR_BYTES + 40, 'a');
  parser.completedPageCount = 7;
  parser.flushPendingAnchor(id.c_str());
  parser.pendingAnchorId = id;
  parser.flushPendingAnchor();
  id[0] = 'b';

  ASSERT_EQ(parser.getAnchors().size(), 2u);
  for (const auto& anchor : parser.getAnchors()) {
    EXPECT_EQ(anchor.length, id.size());
    EXPECT_EQ(anchor.id[0], 'a');
    EXPECT_EQ(anchor.id[anchor.length], '\0');
    EXPECT_EQ(anchor.page, 7u);
  }
  EXPECT_TRUE(parser.pendingAnchorId.empty());
}

TEST_F(ChapterHtmlSlimParserTest, RejectsSectionWhenStoredAnchorCopyFails) {
  const std::string id(80, 'a');
  failNextArrayAllocation = true;
  parser.flushPendingAnchor(id.c_str());

  EXPECT_TRUE(parser.layoutFailed);
  EXPECT_TRUE(parser.getAnchors().empty());
  EXPECT_FALSE(parser.finishParse());
}

TEST_F(ChapterHtmlSlimParserTest, RejectsSectionWhenAnchorSlotsCannotGrow) {
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(parser.appendAnchor("kept", static_cast<uint16_t>(i)));
  }
  ASSERT_EQ(parser.anchorData.size(), parser.anchorData.capacity());
  parser.pendingAnchorId.assign(80, 'a');
  failNextArrayAllocation = true;
  arrayAllocationsToSkip = 1;  // The ID copy succeeds; slot growth fails.
  parser.flushPendingAnchor();

  EXPECT_TRUE(parser.layoutFailed);
  EXPECT_EQ(parser.pendingAnchorId.size(), 80u);
  ASSERT_EQ(parser.getAnchors().size(), 4u);
  for (const auto& anchor : parser.getAnchors()) EXPECT_STREQ(anchor.id.get(), "kept");
  EXPECT_FALSE(parser.finishParse());
}

TEST_F(ChapterHtmlSlimParserTest, RejectsSectionWhenHorizontalRuleAnchorCopyFails) {
  parser.currentPage = std::make_unique<Page>();
  ASSERT_TRUE(parser.currentPage->elements.reserve(4));
  parser.pendingAnchorId.assign(80, 'a');
  failNextArrayAllocation = true;
  parser.emitHorizontalRule(BlockStyle{});

  EXPECT_TRUE(parser.layoutFailed);
  EXPECT_EQ(parser.pendingAnchorId.size(), 80u);
  EXPECT_TRUE(parser.getAnchors().empty());
  EXPECT_FALSE(parser.finishParse());
}

TEST_F(ChapterHtmlSlimParserTest, RejectsSectionWhenFinalAnchorCopyFails) {
  parser.pendingAnchorId.assign(80, 'a');
  bool emitted = false;
  parser.completePageFn = [&](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t) { emitted = true; };
  failNextArrayAllocation = true;

  EXPECT_FALSE(parser.finishParse());
  EXPECT_TRUE(parser.layoutFailed);
  EXPECT_FALSE(emitted);
  EXPECT_TRUE(parser.getAnchors().empty());
}

TEST_F(ChapterHtmlSlimParserTest, KeepsAnchorsAcrossRepeatedStackedStorageOverflow) {
  parser.tableRowStacked = true;
  parser.insideTableCell = true;
  for (size_t i = 0; i < 80; ++i) {
    parser.pendingAnchorId = "paragraph-" + std::to_string(i);
    parser.collectPendingTableAnchor();
    EXPECT_TRUE(parser.pendingAnchorId.empty());
    EXPECT_LE(parser.tableRowAnchorBytes, ChapterHtmlSlimParser::MAX_GRID_TABLE_ANCHOR_BYTES);
  }
  parser.flushTableRowAnchors();
  ASSERT_EQ(parser.anchorData.size(), 80u);
  std::set<std::string> ids;
  for (const auto& anchor : parser.anchorData) ids.insert(anchor.id.get());
  EXPECT_EQ(ids.size(), 80u);
}

TEST_F(ChapterHtmlSlimParserTest, OversizedTocAnchorKeepsBufferedAliasesAfterPageBreak) {
  parser.tableRowStacked = true;
  parser.insideTableCell = true;
  parser.pendingAnchorId = "alias";
  parser.collectPendingTableAnchor();
  const std::string chapter(ChapterHtmlSlimParser::MAX_GRID_TABLE_ANCHOR_BYTES, 'x');
  parser.tocAnchors = {chapter};
  parser.currentPage = std::make_unique<Page>();
  ASSERT_TRUE(parser.currentPage->elements.push_back(std::make_unique<PageHorizontalRule>(100, 1, 0, 0)));
  size_t completed = 0;
  parser.completePageFn = [&](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t) { ++completed; };
  parser.pendingAnchorId = chapter;
  parser.collectPendingTableAnchor();
  parser.flushTableRowAnchors();
  EXPECT_EQ(completed, 1u);
  ASSERT_EQ(parser.anchorData.size(), 2u);
  for (const auto& anchor : parser.anchorData) EXPECT_EQ(anchor.page, 1u);
}

TEST_F(ChapterHtmlSlimParserTest, OverflowingAliasFollowsBufferedTocPageBreak) {
  parser.tableRowStacked = true;
  parser.insideTableCell = true;
  const std::string chapter(ChapterHtmlSlimParser::MAX_GRID_TABLE_ANCHOR_BYTES - 2, 'x');
  parser.tocAnchors = {chapter};
  parser.pendingAnchorId = chapter;
  parser.collectPendingTableAnchor();
  parser.currentPage = std::make_unique<Page>();
  ASSERT_TRUE(parser.currentPage->elements.push_back(std::make_unique<PageHorizontalRule>(100, 1, 0, 0)));
  parser.completePageFn = [](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t) {};
  parser.pendingAnchorId = "alias";
  parser.collectPendingTableAnchor();
  parser.flushTableRowAnchors();
  ASSERT_EQ(parser.anchorData.size(), 2u);
  for (const auto& anchor : parser.anchorData) EXPECT_EQ(anchor.page, 1u);
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
      std::make_unique<TextBlock>(std::vector<std::string>{}, std::vector<int16_t>{},
                                  std::vector<EpdFontFamily::Style>{}, std::vector<uint8_t>{}, std::vector<uint16_t>{});
  parser.addLineToPage(std::move(line), 0);
  EXPECT_EQ(completed, 0u);
  ASSERT_NE(parser.currentPage, nullptr);
  EXPECT_EQ(parser.currentPage->elements.size(), 1u);
}

TEST_F(ChapterHtmlSlimParserTest, MapsCellAliasesAfterItsTocPageBreak) {
  parser.tableRowStacked = true;
  parser.insideTableCell = true;
  parser.tocAnchors = {"chapter"};
  parser.currentPage = std::make_unique<Page>();
  ASSERT_TRUE(parser.currentPage->elements.push_back(std::make_unique<PageHorizontalRule>(100, 1, 0, 0)));
  parser.completePageFn = [&](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t) {
    EXPECT_EQ(parser.pendingAnchorId, "next-cell");
  };
  parser.pendingAnchorId = "alias";
  parser.collectPendingTableAnchor();
  parser.pendingAnchorId = "chapter";
  parser.collectPendingTableAnchor();
  parser.pendingAnchorId = "next-cell";
  parser.flushPendingTableCellAnchors();
  ASSERT_EQ(parser.anchorData.size(), 2u);
  for (const auto& anchor : parser.anchorData) EXPECT_EQ(anchor.page, 1u);
  EXPECT_EQ(parser.pendingAnchorId, "next-cell");
}

TEST_F(ChapterHtmlSlimParserTest, MapsNormalTableAnchorAfterLaterTocAnchor) {
  parser.tocAnchors = {"chapter"};
  parser.currentPage = std::make_unique<Page>();
  ASSERT_TRUE(parser.currentPage->elements.push_back(std::make_unique<PageHorizontalRule>(100, 1, 0, 0)));
  parser.completePageFn = [](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t) {};
  ASSERT_TRUE(parser.tableRowCells.reserve(1));
  auto cell = std::make_unique<ParsedText>(false);
  cell->addWord("chapter", EpdFontFamily::REGULAR);
  ASSERT_TRUE(parser.tableRowCells.push_back(std::move(cell)));

  parser.pendingAnchorId = "alias";
  parser.collectPendingTableAnchor();
  parser.pendingAnchorId = "chapter";
  parser.collectPendingTableAnchor();
  parser.finishTableRow();

  ASSERT_EQ(parser.anchorData.size(), 2u);
  for (const auto& anchor : parser.anchorData) EXPECT_EQ(anchor.page, 1u);
}

TEST_F(ChapterHtmlSlimParserTest, FailsLayoutWhenHorizontalRulePageAllocationFails) {
  allocationSizeToFail = sizeof(Page);
  parser.emitHorizontalRule(BlockStyle{});

  EXPECT_TRUE(parser.layoutFailed);
}

TEST_F(ChapterHtmlSlimParserTest, FailsLayoutWhenHorizontalRuleAllocationFails) {
  allocationSizeToFail = sizeof(PageHorizontalRule);
  parser.emitHorizontalRule(BlockStyle{});

  EXPECT_TRUE(parser.layoutFailed);
}

TEST_F(ChapterHtmlSlimParserTest, DoesNotEmitLineWhenItsArenaAllocationFails) {
  parser.currentTextBlock->addWord("a-word-longer-than-small-string-storage", EpdFontFamily::REGULAR);
  bool emitted = false;
  failNextArrayAllocation = true;
  additionalArrayFailures = 1;  // Fail the cache-eviction retry too.
  EXPECT_FALSE(parser.currentTextBlock->layoutAndExtractLines(
      renderer, 0, 1000, [&](std::unique_ptr<TextBlock>, uint32_t) { emitted = true; }));
  EXPECT_FALSE(emitted);
  EXPECT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_TRUE(parser.currentTextBlock->hadDroppedWords());
}

TEST_F(ChapterHtmlSlimParserTest, RecoversLineWhenArenaRetrySucceeds) {
  parser.currentTextBlock->addWord("cell", EpdFontFamily::REGULAR);
  size_t emitted = 0;
  failNextArrayAllocation = true;
  EXPECT_TRUE(parser.currentTextBlock->layoutAndExtractLines(renderer, 0, 1000,
                                                             [&](std::unique_ptr<TextBlock>, uint32_t) { ++emitted; }));
  EXPECT_EQ(emitted, 1u);
  EXPECT_FALSE(parser.currentTextBlock->hadDroppedWords());
}

TEST_F(ChapterHtmlSlimParserTest, RejectsDroppedWordsAfterGridCellIsBuffered) {
  parser.insideTableCell = true;
  failNextArrayAllocation = true;
  additionalArrayFailures = 1;
  parser.currentTextBlock->addWord("cell", EpdFontFamily::REGULAR);
  ASSERT_TRUE(parser.currentTextBlock->hadDroppedWords());
  parser.closeTableCell();
  EXPECT_EQ(parser.currentTextBlock, nullptr);
  EXPECT_TRUE(parser.layoutOom);
  EXPECT_EQ(parser.parseStep(), ChapterHtmlSlimParser::ParseStatus::Error);
  EXPECT_FALSE(parser.finishParse());
}

TEST_F(ChapterHtmlSlimParserTest, RejectsDroppedWordsAfterStackedCellIsDiscarded) {
  parser.tableRowStacked = true;
  parser.insideTableCell = true;
  failNextArrayAllocation = true;
  additionalArrayFailures = 1;
  parser.currentTextBlock->addWord("cell", EpdFontFamily::REGULAR);
  ASSERT_TRUE(parser.currentTextBlock->hadDroppedWords());
  parser.closeTableCell();
  EXPECT_EQ(parser.currentTextBlock, nullptr);
  EXPECT_TRUE(parser.layoutOom);
  EXPECT_FALSE(parser.finishParse());
}

TEST_F(ChapterHtmlSlimParserTest, DoesNotEmitLineWhenBlockAllocationFails) {
  parser.currentTextBlock->addWord("a-word-longer-than-small-string-storage", EpdFontFamily::REGULAR);
  bool emitted = false;
  allocationSizeToFail = sizeof(TextBlock);
  EXPECT_FALSE(parser.currentTextBlock->layoutAndExtractLines(
      renderer, 0, 1000, [&](std::unique_ptr<TextBlock>, uint32_t) { emitted = true; }));
  EXPECT_FALSE(emitted);
  EXPECT_EQ(parser.currentTextBlock->size(), 1u);
}

TEST_F(ChapterHtmlSlimParserTest, ReportsFailureAfterEmittingEarlierLines) {
  parser.currentTextBlock->addWord("first", EpdFontFamily::REGULAR);
  parser.currentTextBlock->addWord("second", EpdFontFamily::REGULAR);
  size_t emitted = 0;
  EXPECT_FALSE(
      parser.currentTextBlock->layoutAndExtractLines(renderer, 0, 40, [&](std::unique_ptr<TextBlock>, uint32_t) {
        ++emitted;
        allocationSizeToFail = sizeof(TextBlock);
      }));
  EXPECT_EQ(emitted, 1u);
}

TEST_F(ChapterHtmlSlimParserTest, RejectsSectionAfterCharacterDataBlockAllocationFailure) {
  parser.currentTextBlock.reset();
  allocationSizeToFail = sizeof(ParsedText);
  ChapterHtmlSlimParser::characterData(&parser, "caption ", 8);

  EXPECT_EQ(allocationSizeToFail, 0u);
  EXPECT_TRUE(parser.layoutFailed);
  EXPECT_EQ(parser.parseStep(), ChapterHtmlSlimParser::ParseStatus::Error);
  EXPECT_FALSE(parser.finishParse());
}

TEST_F(ChapterHtmlSlimParserTest, RejectsSectionAfterGridCellArenaFailure) {
  ASSERT_TRUE(parser.tableRowCells.reserve(2));
  for (int i = 0; i < 2; ++i) {
    auto cell = std::make_unique<ParsedText>(false);
    cell->addWord("cell", EpdFontFamily::REGULAR);
    ASSERT_TRUE(parser.tableRowCells.push_back(std::move(cell)));
  }
  failNextArrayAllocation = true;
  arrayAllocationsToSkip = 2;  // Visible offsets and the first cell's line slots.
  additionalArrayFailures = 1;
  parser.finishTableRow();
  EXPECT_TRUE(parser.layoutFailed);
  EXPECT_EQ(parser.parseStep(), ChapterHtmlSlimParser::ParseStatus::Error);
  EXPECT_FALSE(parser.finishParse());
}

TEST_F(ChapterHtmlSlimParserTest, RejectsSectionAfterStackedCellArenaFailure) {
  parser.tableRowStacked = true;
  parser.insideTableCell = true;
  parser.currentTextBlock->addWord("cell", EpdFontFamily::REGULAR);
  failNextArrayAllocation = true;
  additionalArrayFailures = 1;
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
  ASSERT_EQ(parser.currentTextBlock->wordAt(0), "Before");
  ASSERT_EQ(parser.currentTextBlock->wordAt(1), "After");
}

TEST_F(ChapterHtmlSlimParserTest, DivWithHiddenAttributeContentShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "div", attributes);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
}

TEST_F(ChapterHtmlSlimParserTest, HiddenIdDoesNotDisplaceVisibleAnchor) {
  parser.pendingAnchorId = "visible";
  const XML_Char* attributes[] = {"id", "hidden-target", "hidden", "", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", attributes);
  EXPECT_EQ(parser.pendingAnchorId, "visible");
  EXPECT_TRUE(parser.anchorData.empty());
}
TEST_F(ChapterHtmlSlimParserTest, DisplayNoneIdDoesNotDisplaceVisibleAnchor) {
  parser.pendingAnchorId = "visible";
  const XML_Char* attributes[] = {"id", "hidden-target", "style", "display:none", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", attributes);
  EXPECT_EQ(parser.pendingAnchorId, "visible");
  EXPECT_TRUE(parser.anchorData.empty());
}

TEST_F(ChapterHtmlSlimParserTest, RejectsSectionAfterTableCellAllocationFailure) {
  parser.tableDepth = 1;
  allocationSizeToFail = sizeof(ParsedText);
  ChapterHtmlSlimParser::startElement(&parser, "td", nullptr);
  allocationSizeToFail = 0;
  EXPECT_TRUE(parser.layoutFailed);
  EXPECT_FALSE(parser.finishParse());
}
TEST_F(ChapterHtmlSlimParserTest, RejectsSectionAfterPageLineAllocationFailure) {
  parser.currentPage = std::make_unique<Page>();
  auto line =
      std::make_unique<TextBlock>(std::vector<std::string>{}, std::vector<int16_t>{},
                                  std::vector<EpdFontFamily::Style>{}, std::vector<uint8_t>{}, std::vector<uint16_t>{});
  allocationSizeToFail = sizeof(PageLine);
  parser.addLineToPage(std::move(line), 0);
  EXPECT_TRUE(parser.layoutFailed);
  EXPECT_TRUE(parser.currentPage->elements.empty());
  EXPECT_FALSE(parser.finishParse());
}

TEST_F(ChapterHtmlSlimParserTest, RejectsSectionAfterGridAllocationFailure) {
  ASSERT_TRUE(parser.tableRowCells.reserve(2));
  for (int i = 0; i < 2; ++i) {
    auto cell = std::make_unique<ParsedText>(false);
    cell->addWord("cell", EpdFontFamily::REGULAR);
    ASSERT_TRUE(parser.tableRowCells.push_back(std::move(cell)));
  }
  allocationSizeToFail = sizeof(PageTableGridRow);
  // Each one-word cell emits one PageLine before the grid allocation.
  matchingAllocationsToSkip = sizeof(PageLine) == sizeof(PageTableGridRow) ? 2 : 0;
  parser.finishTableRow();
  EXPECT_EQ(allocationSizeToFail, 0u);
  EXPECT_TRUE(parser.layoutFailed);
  EXPECT_FALSE(parser.finishParse());
}

TEST_F(ChapterHtmlSlimParserTest, RejectsSectionAfterTableSeparatorAllocationFailure) {
  parser.currentPage = std::make_unique<Page>();
  ASSERT_TRUE(parser.currentPage->elements.reserve(1));
  ASSERT_TRUE(parser.currentPage->elements.push_back(std::make_unique<PageHorizontalRule>(100, 1, 0, 0)));
  parser.tableRowStacked = true;
  allocationSizeToFail = sizeof(PageHorizontalRule);
  parser.finishTableRow();
  EXPECT_TRUE(parser.layoutFailed);
  EXPECT_FALSE(parser.tablePreviousRowEndedWithSeparator);
  EXPECT_EQ(parser.currentPage->elements.size(), 1u);
  EXPECT_FALSE(parser.finishParse());
}
TEST_F(ChapterHtmlSlimParserTest, LayoutBufferGrowthFailurePreservesExistingEntries) {
  LayoutBuffer<std::unique_ptr<int>> buffer;
  ASSERT_TRUE(buffer.reserve(1));
  ASSERT_TRUE(buffer.push_back(std::make_unique<int>(42)));
  const auto* retained = buffer[0].get();
  failNextArrayAllocation = true;
  EXPECT_FALSE(buffer.push_back(std::make_unique<int>(7)));
  ASSERT_EQ(buffer.size(), 1u);
  EXPECT_EQ(buffer.capacity(), 1u);
  EXPECT_EQ(buffer[0].get(), retained);
  EXPECT_EQ(*buffer[0], 42);
  ASSERT_TRUE(buffer.push_back(std::make_unique<int>(7)));
  EXPECT_EQ(buffer.size(), 2u);
  EXPECT_EQ(*buffer[1], 7);
  buffer.clear();
  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(buffer.begin(), buffer.end());
  EXPECT_EQ(buffer.capacity(), 2u);
  ASSERT_TRUE(buffer.push_back(std::make_unique<int>(9)));
  EXPECT_EQ(*buffer[0], 9);
}

TEST_F(ChapterHtmlSlimParserTest, LayoutBufferRejectsOverflow) {
  LayoutBuffer<uint32_t> buffer;
  EXPECT_EQ(buffer.begin(), buffer.end());
  EXPECT_FALSE(buffer.reserve(std::numeric_limits<size_t>::max()));
  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(buffer.capacity(), 0u);
}

TEST_F(ChapterHtmlSlimParserTest, RejectsTableWhenCellSlotsCannotBeAllocated) {
  failNextArrayAllocation = true;
  ChapterHtmlSlimParser::startElement(&parser, "table", nullptr);
  EXPECT_TRUE(parser.layoutFailed);
  EXPECT_FALSE(parser.finishParse());
}

TEST_F(ChapterHtmlSlimParserTest, RejectsGridWhenVisibleOffsetsCannotBeAllocated) {
  ASSERT_TRUE(parser.tableRowCells.reserve(2));
  for (int i = 0; i < 2; ++i) {
    auto cell = std::make_unique<ParsedText>(false);
    cell->addWord("cell", EpdFontFamily::REGULAR);
    ASSERT_TRUE(parser.tableRowCells.push_back(std::move(cell)));
  }
  failNextArrayAllocation = true;
  parser.finishTableRow();
  EXPECT_TRUE(parser.layoutFailed);
  EXPECT_FALSE(parser.finishParse());
}

TEST_F(ChapterHtmlSlimParserTest, RejectsGridWhenLineSlotsCannotBeAllocated) {
  ASSERT_TRUE(parser.tableRowCells.reserve(2));
  for (int i = 0; i < 2; ++i) {
    auto cell = std::make_unique<ParsedText>(false);
    cell->addWord("cell", EpdFontFamily::REGULAR);
    ASSERT_TRUE(parser.tableRowCells.push_back(std::move(cell)));
  }
  failNextArrayAllocation = true;
  arrayAllocationsToSkip = 1;  // Visible offsets succeed; first cell's slots fail.
  parser.finishTableRow();
  EXPECT_TRUE(parser.layoutFailed);
  EXPECT_FALSE(parser.finishParse());
}

TEST_F(ChapterHtmlSlimParserTest, GridElementGrowthFailurePreservesExistingPage) {
  parser.currentPage = std::make_unique<Page>();
  ASSERT_TRUE(parser.currentPage->elements.reserve(1));
  ASSERT_TRUE(parser.currentPage->elements.push_back(std::make_unique<PageHorizontalRule>(100, 1, 0, 0)));
  const auto* retained = parser.currentPage->elements[0].get();
  failNextArrayAllocation = true;
  EXPECT_FALSE(parser.addTableGridSegment(2, 0, 16));
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  EXPECT_EQ(parser.currentPage->elements[0].get(), retained);
}

TEST_F(ChapterHtmlSlimParserTest, RejectsSectionWhenPageLineSlotsCannotGrow) {
  parser.currentPage = std::make_unique<Page>();
  auto line =
      std::make_unique<TextBlock>(std::vector<std::string>{}, std::vector<int16_t>{},
                                  std::vector<EpdFontFamily::Style>{}, std::vector<uint8_t>{}, std::vector<uint16_t>{});
  failNextArrayAllocation = true;
  parser.addLineToPage(std::move(line), 0);
  EXPECT_TRUE(parser.layoutFailed);
  EXPECT_TRUE(parser.currentPage->elements.empty());
  EXPECT_FALSE(parser.finishParse());
}

TEST_F(ChapterHtmlSlimParserTest, PageDeserializeRejectsElementStorageFailure) {
  const auto path = std::filesystem::temp_directory_path() / "crosspoint-page-slot-failure.bin";
  {
    Page page;
    ASSERT_TRUE(page.elements.push_back(std::make_unique<PageTableGridRow>(240, 20, 2, 0, 0)));
    HalFile output;
    ASSERT_TRUE(output.open(path.c_str(), "wb"));
    ASSERT_TRUE(page.serialize(output));
  }
  {
    HalFile input;
    ASSERT_TRUE(input.open(path.c_str(), "rb"));
    failNextArrayAllocation = true;
    EXPECT_EQ(Page::deserialize(input), nullptr);
  }
  std::filesystem::remove(path);
}
}  // namespace

TEST(TextSpacingLayout, TrackingSeparatesCjkTokensAndScalesWordSpaces) {
  GfxRenderer renderer;
  for (bool hyphenation : {false, true}) {
    BlockStyle style;
    style.alignment = CssTextAlign::Left;
    style.textIndentDefined = true;
    ParsedText text(false, hyphenation, false, style);
    text.addWord("一二三", EpdFontFamily::REGULAR);
    text.addWord("四五", EpdFontFamily::REGULAR);
    unsigned lines = 0;
    text.layoutAndExtractLines(
        renderer, 0, 200,
        [&](std::unique_ptr<TextBlock> line, auto) {
          ++lines;
          ASSERT_EQ(line->wordCount(), 5);
          EXPECT_EQ(line->wordXpos(0), 0);
          EXPECT_EQ(line->wordXpos(1), 7);  // 8 px glyph, -1 px tracking
          EXPECT_EQ(line->wordXpos(2), 14);
          EXPECT_EQ(line->wordXpos(3), 28);  // 8 px glyph plus 150% of a 4 px space, no tracking
          EXPECT_EQ(line->wordXpos(4), 35);
        },
        true, -1, 150);
    EXPECT_EQ(lines, 1u);
  }
  EXPECT_EQ(renderer.getTextAdvanceX(0, "ab", EpdFontFamily::REGULAR), 16);
  EXPECT_EQ(renderer.getSpaceWidth(0, EpdFontFamily::REGULAR), 4);
}

TEST(TextSpacingLayout, WordSpacingChangesWrapThreshold) {
  GfxRenderer renderer;
  for (uint8_t percent : {50, 100, 125, 200}) {
    BlockStyle style;
    style.alignment = CssTextAlign::Left;
    style.textIndentDefined = true;
    ParsedText text(false, false, false, style);
    text.addWord("ab", EpdFontFamily::REGULAR);
    text.addWord("cd", EpdFontFamily::REGULAR);
    unsigned lines = 0;
    text.layoutAndExtractLines(renderer, 0, 36, [&](std::unique_ptr<TextBlock>, auto) { ++lines; }, true, 0, percent);
    EXPECT_EQ(lines, percent > 100 ? 2u : 1u);  // 16 + 16 + scaled 4 px space
  }
}

TEST(TextSpacingLayout, CachedPageRestoresSpacing) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.textIndentDefined = true;
  ParsedText text(false, false, false, style);
  text.addWord("一二三", EpdFontFamily::REGULAR);
  text.addWord("四五", EpdFontFamily::REGULAR);
  const auto path = (std::filesystem::temp_directory_path() / "crosspoint-text-spacing.bin").string();
  unsigned lines = 0;
  text.layoutAndExtractLines(
      renderer, 0, 200,
      [&](std::unique_ptr<TextBlock> line, auto) {
        ++lines;
        Page page;
        ASSERT_TRUE(page.elements.push_back(std::make_unique<PageLine>(std::move(line), 4, 12)));
        const auto* original = static_cast<const PageLine&>(*page.elements[0]).getBlock();
        {
          HalFile file;
          ASSERT_TRUE(file.open(path.c_str(), "wb"));
          ASSERT_TRUE(page.serialize(file));
        }
        HalFile file;
        ASSERT_TRUE(file.open(path.c_str(), "rb"));
        auto cachedPage = Page::deserialize(file);
        ASSERT_NE(cachedPage, nullptr);
        ASSERT_EQ(cachedPage->elements.size(), 1);
        const auto* cached = static_cast<const PageLine&>(*cachedPage->elements[0]).getBlock();
        ASSERT_NE(cached, nullptr);
        EXPECT_EQ(cached->getBlockStyle().characterSpacing, -2);
        ASSERT_EQ(cached->wordCount(), 5);
        EXPECT_EQ(cached->wordXpos(3) - cached->wordXpos(2), 10);  // 8 + half-width space
        EXPECT_EQ(file.position(), file.size());
        ASSERT_EQ(cached->wordCount(), original->wordCount());
        for (uint16_t i = 0; i < original->wordCount(); ++i) EXPECT_EQ(cached->wordXpos(i), original->wordXpos(i));
      },
      true, -2, 50);
  EXPECT_EQ(lines, 1u);
  std::filesystem::remove(path);
}

TEST_F(ChapterHtmlSlimParserTest, ParserAppliesTextSpacingToParagraphs) {
  parser.setTextSpacing(-1, 150);
  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  const std::string text = "\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89 \xe5\x9b\x9b\xe4\xba\x94";  // 一二三 四五
  ChapterHtmlSlimParser::characterData(&parser, text.c_str(), static_cast<int>(text.size()));
  ChapterHtmlSlimParser::endElement(&parser, "p");
  parser.makePages();
  ASSERT_NE(parser.currentPage, nullptr);
  unsigned lines = 0;
  for (const auto& element : parser.currentPage->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& block = *static_cast<const PageLine&>(*element).getBlock();
    ++lines;
    ASSERT_EQ(block.wordCount(), 5);
    EXPECT_EQ(block.getBlockStyle().characterSpacing, -1);
    EXPECT_EQ(block.wordXpos(1) - block.wordXpos(0), 7);   // 8 px glyph, -1 px tracking
    EXPECT_EQ(block.wordXpos(3) - block.wordXpos(2), 14);  // glyph plus 150% of a 4 px space
  }
  EXPECT_EQ(lines, 1u);
}

TEST(KoreanLayout, HangulWordsStayWholeAndWrapAtSpaces) {
  GfxRenderer renderer;
  {
    BlockStyle style;
    style.alignment = CssTextAlign::Left;
    style.textIndentDefined = true;
    ParsedText text(false, false, false, style);
    text.addWord("가나다", EpdFontFamily::REGULAR);
    text.addWord("라마", EpdFontFamily::REGULAR);
    text.addWord("3개를", EpdFontFamily::REGULAR);
    text.addWord("iPhone을", EpdFontFamily::REGULAR);
    std::vector<std::vector<std::string>> lines;
    text.layoutAndExtractLines(renderer, 0, 60, [&](std::unique_ptr<TextBlock> line, auto) {
      auto& words = lines.emplace_back();
      for (uint16_t i = 0; i < line->wordCount(); ++i) words.emplace_back(line->wordText(i));
    });
    // 가나다 라마 is 24 + 4 + 16 px; adding 3개를 would need 72 px, and no break exists inside it.
    const std::vector<std::vector<std::string>> expected{{"가나다", "라마"}, {"3개를"}, {"iPhone을"}};
    EXPECT_EQ(lines, expected);
  }
}

TEST(KoreanLayout, JustifiedHangulStretchesOnlyWordSpaces) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  ParsedText text(false, false, false, style);
  for (const char* word : {"가나", "다라", "마바", "사아"}) text.addWord(word, EpdFontFamily::REGULAR);
  unsigned lines = 0;
  text.layoutAndExtractLines(renderer, 0, 60, [&](std::unique_ptr<TextBlock> line, auto) {
    if (lines++ != 0) return;
    // 3 x 16 px words + 2 x 4 px spaces leave 4 px, split across the two spaces only.
    ASSERT_EQ(line->wordCount(), 3);
    EXPECT_EQ(line->wordXpos(0), 0);
    EXPECT_EQ(line->wordXpos(1), 22);
    EXPECT_EQ(line->wordXpos(2), 44);
  });
  EXPECT_EQ(lines, 2u);
}

TEST(KoreanLayout, HangulGluedAcrossInlineStyleIsUnbreakable) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  ParsedText text(false, false, false, style);
  text.addWord("가나", EpdFontFamily::REGULAR);
  text.addWord("한국", EpdFontFamily::REGULAR);
  text.addWord("어", EpdFontFamily::BOLD, false, /*attachToPrevious=*/true);
  std::vector<std::vector<std::string>> lines;
  text.layoutAndExtractLines(renderer, 0, 40, [&](std::unique_ptr<TextBlock> line, auto) {
    auto& words = lines.emplace_back();
    for (uint16_t i = 0; i < line->wordCount(); ++i) words.emplace_back(line->wordText(i));
  });
  // 가나 한국 fits in 36 px, but 어 is glued to 한국, so the whole word moves down.
  const std::vector<std::vector<std::string>> expected{{"가나"}, {"한국", "어"}};
  EXPECT_EQ(lines, expected);
}
