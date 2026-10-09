#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "src/activities/settings/TextSettingsPreview.h"
#include "src/util/ParagraphIndentMigration.h"

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class
#include "../../src/fontIds.h"

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

  void SetUp() override { parser.currentTextBlock = std::make_unique<ParsedText>(); }
};

TEST_F(ChapterHtmlSlimParserTest, RubySurvivesPartialParagraphExtraction) {
  ParsedText text;
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
  parser.tableRowCells.reserve(2);
  std::multiset<std::string> expected;
  for (int column = 0; column < 2; ++column) {
    auto cell = std::make_unique<ParsedText>();
    for (int index = 0; index < (column == 0 ? 30 : 3); ++index) {
      const auto word = std::string(column == 0 ? "left" : "right") + std::to_string(index);
      expected.insert(word);
      cell->addWord(word, EpdFontFamily::REGULAR);
    }
    if (column == 0) cell->setRubyGroupAt(0, 2, "reading");
    parser.tableRowCells.push_back(std::move(cell));
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

TEST_F(ChapterHtmlSlimParserTest, BlockFontScaleSnapsToBuiltinSizes) {
  for (const int id : {NOTOSERIF_12_FONT_ID, NOTOSERIF_14_FONT_ID, NOTOSERIF_16_FONT_ID, NOTOSERIF_18_FONT_ID}) {
    renderer.fontMap.emplace(id, EpdFontFamily(nullptr));
  }
  parser.fontId = NOTOSERIF_14_FONT_ID;
  EXPECT_EQ(parser.fontIdForScale(1.0f), NOTOSERIF_14_FONT_ID);
  EXPECT_EQ(parser.fontIdForScale(2.0f), NOTOSERIF_18_FONT_ID);
  EXPECT_EQ(parser.fontIdForScale(1.17f), NOTOSERIF_16_FONT_ID);
  EXPECT_EQ(parser.fontIdForScale(0.83f), NOTOSERIF_12_FONT_ID);
  parser.fontId = NOTOSANS_14_FONT_ID;  // family not registered with the renderer
  EXPECT_EQ(parser.fontIdForScale(2.0f), NOTOSANS_14_FONT_ID);
  parser.fontId = 12345;  // SD or vector font: never rescaled
  EXPECT_EQ(parser.fontIdForScale(2.0f), 12345);
}

TEST_F(ChapterHtmlSlimParserTest, BlockFontScaleInheritsAndUsesHeadingDefaults) {
  parser.blockStyleStack.assign(1, BlockStyle{});
  BlockStyle heading;
  parser.applyBlockFontScale(heading, CssStyle{}, "h2");
  EXPECT_FLOAT_EQ(heading.fontScale, 1.5f);

  CssStyle css;
  css.fontSize = CssLength{0.5f, CssUnit::Em};
  css.defined.fontSize = 1;
  parser.blockStyleStack.push_back(heading);
  BlockStyle child;
  parser.applyBlockFontScale(child, css, "p");
  EXPECT_FLOAT_EQ(child.fontScale, 0.75f);

  css.fontSize = CssLength{1.2f, CssUnit::Rem};
  parser.applyBlockFontScale(child, css, "p");
  EXPECT_FLOAT_EQ(child.fontScale, 1.2f);

  BlockStyle plain;
  const BlockStyle inherited = heading.getCombinedBlockStyle(plain, BlockStyle::CombineAxis::Horizontal);
  EXPECT_FLOAT_EQ(inherited.fontScale, 1.5f);
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

TEST_F(ChapterHtmlSlimParserTest, SmallCapsSpanMarksWordsWithoutChangingText) {
  const XML_Char* attributes[] = {"style", "font-variant: small-caps", nullptr};
  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Plain ", 6);
  ChapterHtmlSlimParser::startElement(&parser, "span", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "Chapter", 7);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ChapterHtmlSlimParser::characterData(&parser, " after", 6);
  ChapterHtmlSlimParser::endElement(&parser, "p");

  ASSERT_EQ(parser.currentTextBlock->size(), 0u);  // flushed to the page on </p>
  ASSERT_NE(parser.currentPage, nullptr);
  const auto& block = *static_cast<const PageLine&>(*parser.currentPage->elements.front()).getBlock();
  ASSERT_EQ(block.wordCount(), 3);
  EXPECT_STREQ(block.wordText(1), "Chapter");
  EXPECT_EQ(block.wordStyle(0) & EpdFontFamily::SMALL_CAPS, 0);
  EXPECT_NE(block.wordStyle(1) & EpdFontFamily::SMALL_CAPS, 0);
  EXPECT_EQ(block.wordStyle(2) & EpdFontFamily::SMALL_CAPS, 0);
}

class ChapterPageBreakTest : public ChapterHtmlSlimParserTest {
 protected:
  unsigned pages = 0;
  void SetUp() override {
    ChapterHtmlSlimParserTest::SetUp();
    parser.completePageFn = [this](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t) { ++pages; };
    parser.beginParse();
  }
  void paragraph(const char* text, const char* style = nullptr) {
    const XML_Char* attributes[] = {"style", style, nullptr};
    ChapterHtmlSlimParser::startElement(&parser, "p", style ? attributes : nullptr);
    ChapterHtmlSlimParser::characterData(&parser, text, static_cast<int>(strlen(text)));
    ChapterHtmlSlimParser::endElement(&parser, "p");
  }
};

TEST_F(ChapterPageBreakTest, BreakBeforeStartsNewPage) {
  paragraph("one");
  paragraph("two");
  EXPECT_EQ(pages, 0u);
  paragraph("three", "page-break-before: always");
  EXPECT_EQ(pages, 1u);
}

TEST_F(ChapterPageBreakTest, BreakAfterAppliesToFollowingContent) {
  paragraph("one", "break-after: page");
  EXPECT_EQ(pages, 0u);
  paragraph("two");
  EXPECT_EQ(pages, 1u);
}

TEST_F(ChapterPageBreakTest, AvoidAndEmptyPageDoNotBreak) {
  paragraph("first", "page-break-before: always");
  paragraph("second", "page-break-before: avoid");
  EXPECT_EQ(pages, 0u);
}

TEST_F(ChapterPageBreakTest, ContainerBreaksOnceBeforeItsFirstChild) {
  paragraph("zero");
  const XML_Char* attributes[] = {"style", "page-break-before: always", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", attributes);
  paragraph("a");
  paragraph("b");
  ChapterHtmlSlimParser::endElement(&parser, "div");
  paragraph("c");
  EXPECT_EQ(pages, 1u);
}

class ChapterBlockDecorationTest : public ChapterHtmlSlimParserTest {
 protected:
  std::vector<std::unique_ptr<Page>> pages;
  void SetUp() override {
    ChapterHtmlSlimParserTest::SetUp();
    parser.completePageFn = [this](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) {
      pages.push_back(std::move(page));
    };
    parser.beginParse();
  }
  void open(const char* tag, const char* style) {
    const XML_Char* attributes[] = {"style", style, nullptr};
    ChapterHtmlSlimParser::startElement(&parser, tag, style ? attributes : nullptr);
  }
  void text(const std::string& value) {
    ChapterHtmlSlimParser::characterData(&parser, value.c_str(), static_cast<int>(value.size()));
  }
  void close(const char* tag) { ChapterHtmlSlimParser::endElement(&parser, tag); }
  static std::string words(int count) {
    std::string result;
    for (int i = 0; i < count; ++i) result += "word ";
    return result;
  }
  std::vector<const PageElement*> elementsWithTag(PageElementTag tag) {
    std::vector<const PageElement*> found;
    if (parser.currentPage) pages.push_back(std::move(parser.currentPage));
    for (const auto& page : pages) {
      for (const auto& element : page->elements) {
        if (element->getTag() == tag) found.push_back(element.get());
      }
    }
    return found;
  }
};

TEST_F(ChapterBlockDecorationTest, RepeatedSectionBreaksEachAddOneLine) {
  for (int breaks = 1; breaks <= 3; ++breaks) {
    open("p", "margin-top: 1px");
    text("Before");
    close("p");
    const int before = parser.currentPageNextY;
    for (int i = 0; i < breaks; ++i) {
      const XML_Char* attrs[] = {"class", "section-br", "style", "display: block", nullptr};
      ChapterHtmlSlimParser::startElement(&parser, "br", attrs);
      close("br");
    }
    EXPECT_EQ(parser.currentPageNextY - before, breaks * renderer.getLineHeight(0));
    open("p", "margin-top: 1px");
    text("After");
    close("p");
  }
}

TEST_F(ChapterBlockDecorationTest, InlineBreakDoesNotAddParagraphSpacing) {
  parser.extraParagraphSpacing = true;
  open("p", "margin-top: 1px; margin-bottom: 9px");
  text("First");
  open("br", nullptr);
  close("br");
  text("Second");
  close("p");
  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[1]->yPos - lines[0]->yPos, renderer.getLineHeight(0));
}

TEST_F(ChapterBlockDecorationTest, LeadingBreakDoesNotAddBlankSpace) {
  open("br", nullptr);
  close("br");
  open("p", nullptr);
  text("Start");
  close("p");
  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0]->yPos, 0);
}

TEST_F(ChapterBlockDecorationTest, BreakAfterHeadingAddsOneLine) {
  open("h2", nullptr);
  text("Heading");
  close("h2");
  const int before = parser.currentPageNextY;
  open("br", nullptr);
  close("br");
  EXPECT_EQ(parser.currentPageNextY - before, renderer.getLineHeight(0));
}

TEST_F(ChapterBlockDecorationTest, PreWrapPreservesSpacesAndLineBreaksAcrossCallbacks) {
  parser.extraParagraphSpacing = true;
  open("p", "white-space: pre-wrap; text-align: justify");
  text("  first");
  text("  word\n");
  text("second");
  close("p");
  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_EQ(lines.size(), 2u);
  const auto& first = *static_cast<const PageLine*>(lines[0])->getBlock();
  ASSERT_EQ(first.wordCount(), 6);
  EXPECT_STREQ(first.wordText(0), " ");
  EXPECT_STREQ(first.wordText(1), " ");
  EXPECT_STREQ(first.wordText(2), "first");
  EXPECT_STREQ(first.wordText(3), " ");
  EXPECT_STREQ(first.wordText(4), " ");
  EXPECT_STREQ(first.wordText(5), "word");
  // Literal spaces use the font's space advance, with no inserted word gaps.
  EXPECT_EQ(first.wordXpos(2) - first.wordXpos(0), 2 * renderer.getSpaceWidth(0, EpdFontFamily::REGULAR));
  EXPECT_EQ(lines[1]->yPos - lines[0]->yPos, renderer.getLineHeight(0));
}

TEST_F(ChapterBlockDecorationTest, BlockEmphasisIsInheritedOverriddenAndRestored) {
  open("blockquote", "font-style: italic");
  open("p", nullptr);
  text("Inherited");
  close("p");
  open("p", "font-style: normal");
  text("Normal");
  close("p");
  open("p", nullptr);
  text("Restored");
  close("p");
  close("blockquote");
  open("p", nullptr);
  text("Outside");
  close("p");
  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_EQ(lines.size(), 4u);
  for (size_t i = 0; i < lines.size(); ++i) {
    const auto& block = *static_cast<const PageLine*>(lines[i])->getBlock();
    EXPECT_EQ((block.wordStyle(0) & EpdFontFamily::ITALIC) != 0, i == 0 || i == 2);
  }
}

TEST_F(ChapterBlockDecorationTest, PreElementPreservesNewlinesWithoutLeakingToFollowingParagraph) {
  open("pre", nullptr);
  text("one\ntwo");
  close("pre");
  open("p", nullptr);
  text("three\nfour");
  close("p");
  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_EQ(lines.size(), 3u);
  const auto& last = *static_cast<const PageLine*>(lines[2])->getBlock();
  ASSERT_EQ(last.wordCount(), 2);
  EXPECT_STREQ(last.wordText(0), "three");
  EXPECT_STREQ(last.wordText(1), "four");
}

TEST_F(ChapterBlockDecorationTest, FloatedLeadingSpanBecomesDropCap) {
  open("p", nullptr);
  open("span", "float: left; font-size: 3em");
  text("T");
  close("span");
  text("he " + words(60));
  close("p");

  const auto caps = elementsWithTag(TAG_PageDropCap);
  ASSERT_EQ(caps.size(), 1u);
  EXPECT_STREQ(static_cast<const PageDropCap*>(caps[0])->getText(), "T");
  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_GT(lines.size(), 4u);
  // Three lines wrap beside the letter, then text returns to the margin.
  EXPECT_GT(lines[0]->xPos, 0);
  EXPECT_EQ(lines[2]->xPos, lines[0]->xPos);
  EXPECT_EQ(lines[3]->xPos, 0);
  EXPECT_STREQ(static_cast<const PageLine*>(lines[0])->getBlock()->wordText(0), "he");
}

TEST_F(ChapterBlockDecorationTest, MultiWordSpanStaysText) {
  open("p", nullptr);
  open("span", "float: left");
  text("Once upon");
  close("span");
  text(" a time");
  close("p");
  EXPECT_TRUE(elementsWithTag(TAG_PageDropCap).empty());
  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_STREQ(static_cast<const PageLine*>(lines[0])->getBlock()->wordText(0), "Once");
}

TEST_F(ChapterBlockDecorationTest, FirstLetterRuleCapturesQuoteAndLetter) {
  const auto cssPath = std::filesystem::temp_directory_path() / "crosspoint-first-letter.css";
  {
    HalFile output;
    ASSERT_TRUE(output.open(cssPath.c_str(), "wb"));
    const std::string css = "p.opening::first-letter { initial-letter: 2; }";
    output.write(css.data(), css.size());
  }
  HalFile input;
  ASSERT_TRUE(input.open(cssPath.c_str(), "rb"));
  ASSERT_EQ(cssParser.loadFromStream(input), CssParser::ParseResult::Complete);

  const XML_Char* attributes[] = {"class", "opening", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", attributes);
  text("\xe2\x80\x9cIt was " + words(40));
  close("p");
  open("p", nullptr);
  text("Next paragraph");
  close("p");

  const auto caps = elementsWithTag(TAG_PageDropCap);
  ASSERT_EQ(caps.size(), 1u);
  EXPECT_STREQ(static_cast<const PageDropCap*>(caps[0])->getText(), "\xe2\x80\x9cI");
  std::filesystem::remove(cssPath);
}

TEST_F(ChapterBlockDecorationTest, BorderedBlockFramesItsTextAndInsetsIt) {
  open("div", "border: 2px solid black");
  open("p", nullptr);
  text("Framed text");
  close("p");
  close("div");
  open("p", nullptr);
  text("Outside");
  close("p");

  const auto boxes = elementsWithTag(TAG_PageBorderBox);
  ASSERT_EQ(boxes.size(), 1u);
  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_GE(lines[0]->xPos, 6);  // 2 px border + 4 px gap
  EXPECT_GT(lines[0]->yPos, boxes[0]->yPos);
  EXPECT_EQ(lines[1]->xPos, 0);
}

TEST_F(ChapterBlockDecorationTest, EmptyBorderedBlockDrawsARule) {
  open("p", nullptr);
  text("Above");
  close("p");
  open("div", "border-top: 1px solid");
  close("div");
  open("p", nullptr);
  text("Below");
  close("p");
  const auto boxes = elementsWithTag(TAG_PageBorderBox);
  ASSERT_EQ(boxes.size(), 1u);
  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_GT(boxes[0]->yPos, lines[0]->yPos);
  EXPECT_GT(lines[1]->yPos, boxes[0]->yPos);
}

TEST_F(ChapterBlockDecorationTest, ShadedBoxSplitsAcrossPages) {
  parser.viewportHeight = 100;
  open("div", "background-color: #ddd");
  for (int i = 0; i < 12; ++i) {
    open("p", nullptr);
    text("line");
    close("p");
  }
  close("div");
  EXPECT_GE(pages.size(), 1u);
  EXPECT_GE(elementsWithTag(TAG_PageBorderBox).size(), 2u);
}

class ChapterTableBorderTest : public ChapterBlockDecorationTest {
 protected:
  void table(const XML_Char** tableAttributes, const char* cellStyle) {
    ChapterHtmlSlimParser::startElement(&parser, "table", tableAttributes);
    ChapterHtmlSlimParser::startElement(&parser, "tr", nullptr);
    for (const char* cell : {"alpha", "beta"}) {
      open("td", cellStyle);
      text(cell);
      close("td");
    }
    close("tr");
    close("table");
  }
};

TEST_F(ChapterTableBorderTest, BorderAttributeFramesEveryCell) {
  const XML_Char* attributes[] = {"border", "1", nullptr};
  table(attributes, nullptr);
  EXPECT_EQ(elementsWithTag(TAG_PageBorderBox).size(), 2u);
  EXPECT_TRUE(elementsWithTag(TAG_PageHorizontalRule).empty());
}

TEST_F(ChapterTableBorderTest, CellCssBorderFramesEveryCell) {
  table(nullptr, "border: 1px solid black");
  const auto boxes = elementsWithTag(TAG_PageBorderBox);
  ASSERT_EQ(boxes.size(), 2u);
  EXPECT_EQ(boxes[0]->yPos, boxes[1]->yPos);
  EXPECT_LT(boxes[0]->xPos, boxes[1]->xPos);
}

TEST_F(ChapterTableBorderTest, UnborderedTableKeepsRowRule) {
  table(nullptr, nullptr);
  EXPECT_TRUE(elementsWithTag(TAG_PageBorderBox).empty());
  EXPECT_EQ(elementsWithTag(TAG_PageHorizontalRule).size(), 1u);
}

TEST_F(ChapterTableBorderTest, RichCellsPreserveParagraphStylesAndFollowingFlow) {
  parser.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::None);  // Book's Style honors CSS alignment.
  const size_t initialDepth = parser.blockStyleStack.size();
  open("table", nullptr);
  open("tr", nullptr);
  open("td", nullptr);
  text("Firstcell");
  close("td");
  open("td", nullptr);
  open("p", "margin-left: 24px; font-style: italic; font-size: 0.75em");
  EXPECT_TRUE(parser.tableRowStacked);
  EXPECT_TRUE(parser.effectiveItalic);
  EXPECT_FLOAT_EQ(parser.currentTextBlock->getBlockStyle().fontScale, 0.75f);
  text("Entryone");
  close("p");
  open("p", "text-align: right");
  EXPECT_FALSE(parser.effectiveItalic);
  text("Entrytwo");
  close("p");
  close("td");
  close("tr");
  close("table");
  EXPECT_EQ(parser.blockStyleStack.size(), initialDepth);
  open("p", nullptr);
  text("Outside");
  close("p");

  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_EQ(lines.size(), 4u);
  const char* expected[] = {"Firstcell", "Entryone", "Entrytwo", "Outside"};
  for (size_t i = 0; i < lines.size(); ++i) {
    EXPECT_STREQ(static_cast<const PageLine*>(lines[i])->getBlock()->wordText(0), expected[i]);
    if (i > 0) EXPECT_GT(lines[i]->yPos, lines[i - 1]->yPos);
  }
  EXPECT_GE(lines[1]->xPos, 24);
  const auto& left = *static_cast<const PageLine*>(lines[1])->getBlock();
  const auto& right = *static_cast<const PageLine*>(lines[2])->getBlock();
  EXPECT_GT(lines[2]->xPos + right.wordXpos(0), lines[1]->xPos + left.wordXpos(0));
  EXPECT_EQ(lines[3]->xPos, 0);
}

TEST_F(ChapterTableBorderTest, CellImagesUseNormalImageHandlingAndRespectDisplayNone) {
  parser.imageRendering = 1;  // Exercise the shared image fallback without a decoder.
  open("table", nullptr);
  open("tr", nullptr);
  open("td", nullptr);
  const XML_Char* hidden[] = {"style", "display:none", "alt", "hidden", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "img", hidden);
  close("img");
  EXPECT_FALSE(parser.tableRowStacked);
  const XML_Char* visible[] = {"alt", "skull", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "img", visible);
  close("img");
  EXPECT_TRUE(parser.tableRowStacked);
  close("td");
  close("tr");
  close("table");

  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_EQ(lines.size(), 1u);
  const auto* block = static_cast<const PageLine*>(lines[0])->getBlock();
  ASSERT_EQ(block->wordCount(), 2u);
  EXPECT_STREQ(block->wordText(0), "[Image:");
  EXPECT_STREQ(block->wordText(1), "skull]");
}

class ChapterInlineSizeTest : public ChapterBlockDecorationTest {
 protected:
  void SetUp() override {
    ChapterBlockDecorationTest::SetUp();
    for (const int id : {NOTOSERIF_12_FONT_ID, NOTOSERIF_14_FONT_ID, NOTOSERIF_16_FONT_ID, NOTOSERIF_18_FONT_ID}) {
      renderer.fontMap.emplace(id, EpdFontFamily(nullptr));
    }
    parser.fontId = NOTOSERIF_14_FONT_ID;
  }
  int firstLineFontId() {
    const auto lines = elementsWithTag(TAG_PageLine);
    return lines.empty() ? -1 : static_cast<const PageLine*>(lines[0])->getBlock()->getBlockStyle().fontId;
  }
};

TEST_F(ChapterInlineSizeTest, SpanHoldingWholeBlockSizesIt) {
  open("p", nullptr);
  open("span", "font-size: 2em");
  text("Chapter One");
  close("span");
  text(" ");
  close("p");
  EXPECT_EQ(firstLineFontId(), NOTOSERIF_18_FONT_ID);
}

TEST_F(ChapterInlineSizeTest, ShortSizedHeadingSpanFallsBackToStyledText) {
  open("h1", "font-size: 1em; font-style: italic");
  open("span", "font-size: 2em");
  text("II");
  close("span");
  close("h1");
  open("p", nullptr);
  text("After");
  close("p");

  EXPECT_TRUE(elementsWithTag(TAG_PageDropCap).empty());
  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_EQ(lines.size(), 2u);
  const auto& heading = *static_cast<const PageLine*>(lines[0])->getBlock();
  ASSERT_EQ(heading.wordCount(), 1u);
  EXPECT_STREQ(heading.wordText(0), "II");
  EXPECT_EQ(heading.getBlockStyle().fontId, NOTOSERIF_18_FONT_ID);
  EXPECT_EQ(heading.wordStyle(0), EpdFontFamily::BOLD | EpdFontFamily::ITALIC);
  const auto& following = *static_cast<const PageLine*>(lines[1])->getBlock();
  ASSERT_EQ(following.wordCount(), 1u);
  EXPECT_STREQ(following.wordText(0), "After");
  EXPECT_EQ(following.wordStyle(0), EpdFontFamily::REGULAR);
  EXPECT_EQ(following.wordFontId(0, NOTOSERIF_14_FONT_ID), NOTOSERIF_14_FONT_ID);
}

TEST_F(ChapterInlineSizeTest, NestedSpansCompoundTheirSizes) {
  open("p", nullptr);
  open("span", "font-size: 0.9em");
  open("span", "font-size: 0.9em");
  text("small print");
  close("span");
  close("span");
  close("p");
  EXPECT_EQ(firstLineFontId(), NOTOSERIF_12_FONT_ID);
}

TEST_F(ChapterInlineSizeTest, TextAfterTheSpanKeepsBodySize) {
  open("p", nullptr);
  open("span", "font-size: 2em");
  text("Lead");
  close("span");
  text(" and the rest of the paragraph");
  close("p");
  EXPECT_EQ(firstLineFontId(), 0);
  // The lead words keep their own size inline.
  const auto& block = *static_cast<const PageLine*>(elementsWithTag(TAG_PageLine)[0])->getBlock();
  EXPECT_EQ(block.wordFontId(0, NOTOSERIF_14_FONT_ID), NOTOSERIF_18_FONT_ID);
  EXPECT_EQ(block.wordFontId(1, NOTOSERIF_14_FONT_ID), NOTOSERIF_14_FONT_ID);
}

TEST_F(ChapterInlineSizeTest, MixedSizesWithinALineSurviveTheCache) {
  open("p", nullptr);
  text("Normal ");
  open("span", "font-size: 0.8em");
  text("small");
  close("span");
  text(" then ");
  open("big", nullptr);
  text("big");
  close("big");
  close("p");
  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_EQ(lines.size(), 1u);
  const auto& block = *static_cast<const PageLine*>(lines[0])->getBlock();
  ASSERT_EQ(block.wordCount(), 4);
  EXPECT_EQ(block.wordFontId(0, NOTOSERIF_14_FONT_ID), NOTOSERIF_14_FONT_ID);
  EXPECT_EQ(block.wordFontId(1, NOTOSERIF_14_FONT_ID), NOTOSERIF_12_FONT_ID);
  EXPECT_EQ(block.wordFontId(3, NOTOSERIF_14_FONT_ID), NOTOSERIF_16_FONT_ID);

  const auto path = std::filesystem::temp_directory_path() / "crosspoint-word-fonts.bin";
  {
    HalFile output;
    ASSERT_TRUE(output.open(path.c_str(), "wb"));
    ASSERT_TRUE(block.serialize(output));
  }
  HalFile input;
  ASSERT_TRUE(input.open(path.c_str(), "rb"));
  const auto cached = TextBlock::deserialize(input);
  ASSERT_NE(cached, nullptr);
  for (uint16_t i = 0; i < block.wordCount(); ++i) {
    EXPECT_EQ(cached->wordFontId(i, NOTOSERIF_14_FONT_ID), block.wordFontId(i, NOTOSERIF_14_FONT_ID));
  }
  std::filesystem::remove(path);
}

TEST_F(ChapterBlockDecorationTest, SdAndVectorFontsSizeThroughTheVariantProvider) {
  constexpr int SD_FONT = 7;  // not a built-in ladder font
  parser.fontId = SD_FONT;
  renderer.variantsEnabled = true;
  open("h1", nullptr);
  text("Title");
  close("h1");
  open("p", nullptr);
  text("Body ");
  open("span", "font-size: 0.8em");
  text("small");
  close("span");
  close("p");

  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(static_cast<const PageLine*>(lines[0])->getBlock()->getBlockStyle().fontId, SD_FONT * 1000 + 200);
  const auto& body = *static_cast<const PageLine*>(lines[1])->getBlock();
  EXPECT_EQ(body.wordFontId(0, SD_FONT), SD_FONT);
  EXPECT_EQ(body.wordFontId(1, SD_FONT), SD_FONT * 1000 + 80);
}

TEST_F(ChapterBlockDecorationTest, UnloadableVariantFallsBackToSectionFont) {
  EXPECT_EQ(TextBlock::renderFontId(renderer, 12345, 7), 7);
  renderer.variantsEnabled = true;
  EXPECT_EQ(TextBlock::renderFontId(renderer, 12345, 7), 12345);
}

TEST_F(ChapterInlineSizeTest, UniformLinesCarryNoWordFonts) {
  open("p", nullptr);
  text("Plain text only");
  close("p");
  const auto& block = *static_cast<const PageLine*>(elementsWithTag(TAG_PageLine)[0])->getBlock();
  EXPECT_EQ(block.wordFontId(1, NOTOSERIF_14_FONT_ID), NOTOSERIF_14_FONT_ID);
  EXPECT_EQ(block.extraAscent(renderer, NOTOSERIF_14_FONT_ID), 0);
}

TEST_F(ChapterTableBorderTest, ScansColumnWidthsFromMarkup) {
  const auto path = std::filesystem::temp_directory_path() / "crosspoint-table-scan.xhtml";
  {
    HalFile output;
    ASSERT_TRUE(output.open(path.c_str(), "wb"));
    const std::string html =
        "<table class=\"t\"><tr><th>ID</th><td>A much longer description of the item</td></tr>"
        "<tr><td colspan=\"2\">Spanning note that must not widen anything at all</td></tr>"
        "<tr><td>7</td><td>short<br/>lines &amp; more</td></tr></table><p>after</p>";
    output.write(html.data(), html.size());
  }
  HalFile input;
  ASSERT_TRUE(input.open(path.c_str(), "rb"));
  ChapterHtmlSlimParser::TableColumnMeasure measure;
  ASSERT_TRUE(parser.measureTableColumns(input, measure));
  EXPECT_EQ(measure.columns, 2);
  EXPECT_EQ(measure.minWidth[0], 16);  // "ID"
  EXPECT_EQ(measure.minWidth[1], 88);  // "description"
  EXPECT_EQ(measure.prefWidth[1], 31 * 8 + 6 * 4);

  parser.planTableColumns(measure);
  ASSERT_EQ(parser.tableColumnCount, 2);
  EXPECT_EQ(parser.tableColumnWidths[0] + parser.tableColumnWidths[1], parser.viewportWidth);
  EXPECT_LT(parser.tableColumnWidths[0], parser.tableColumnWidths[1]);

  parser.viewportWidth = 200;  // too narrow for natural widths: column 0 keeps its word
  parser.planTableColumns(measure);
  EXPECT_EQ(parser.tableColumnWidths[0], 16 + 8);
  EXPECT_EQ(parser.tableColumnWidths[1], 200 - 24);
  std::filesystem::remove(path);
}

TEST_F(ChapterTableBorderTest, RowsUsePlannedColumnWidths) {
  ChapterHtmlSlimParser::startElement(&parser, "table", nullptr);
  parser.tableColumnWidths = {100, 380};
  parser.tableColumnCount = 2;
  ChapterHtmlSlimParser::startElement(&parser, "tr", nullptr);
  for (const char* cell : {"alpha", "beta"}) {
    open("td", nullptr);
    text(cell);
    close("td");
  }
  close("tr");
  close("table");
  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0]->xPos, 4);
  EXPECT_EQ(lines[1]->xPos, 104);
}

TEST_F(ChapterTableBorderTest, CssCellPaddingInsetsContentAndReservesRowHeight) {
  open("table", nullptr);
  parser.tableColumnWidths = {100, 380};
  parser.tableColumnCount = 2;
  open("tr", nullptr);
  open("td", "padding: 9px 17px 13px 11px; text-align: right");
  text("alpha");
  close("td");
  open("td", "padding: 3px 0 5px");
  text("beta");
  close("td");
  close("tr");
  close("table");
  const int rowBottom = parser.currentPageNextY;
  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0]->xPos, 11);
  EXPECT_EQ(lines[1]->xPos, 100);
  EXPECT_EQ(lines[0]->yPos, 9);
  EXPECT_EQ(lines[1]->yPos, 3);
  const auto& first = *static_cast<const PageLine*>(lines[0])->getBlock();
  EXPECT_EQ(lines[0]->xPos + first.wordXpos(0) + renderer.getTextAdvanceX(0, "alpha", EpdFontFamily::REGULAR), 83);
  EXPECT_GE(rowBottom, 9 + renderer.getLineHeight(0) + 13);
}

TEST_F(ChapterTableBorderTest, PaddedRowsBreakBeforeTheBottomPaddingOverflows) {
  parser.viewportHeight = 48;
  table(nullptr, "padding: 8px");
  table(nullptr, "padding: 8px");
  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_EQ(lines.size(), 4u);
  ASSERT_EQ(pages.size(), 2u);
  for (const auto* line : lines) {
    EXPECT_EQ(line->yPos, 8);
    EXPECT_LE(line->yPos + renderer.getLineHeight(0) + 8, parser.viewportHeight);
  }
}

TEST_F(ChapterTableBorderTest, StackedCellPaddingSurroundsChildParagraphs) {
  const size_t initialDepth = parser.blockStyleStack.size();
  open("table", nullptr);
  open("tr", nullptr);
  open("td", "padding: 7px 8px 9px 10px");
  open("p", nullptr);
  text("Inside");
  close("p");
  close("td");
  close("tr");
  close("table");
  EXPECT_EQ(parser.blockStyleStack.size(), initialDepth);
  const int rowBottom = parser.currentPageNextY;
  open("p", nullptr);
  text("Outside");
  close("p");
  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0]->xPos, 10);
  EXPECT_EQ(lines[0]->yPos, 7);
  EXPECT_GE(rowBottom, 7 + renderer.getLineHeight(0) + 9);
  EXPECT_EQ(lines[1]->xPos, 0);
}

TEST_F(ChapterTableBorderTest, LongTwoColumnFixtureKeepsColumnsAcrossPages) {
  parser.viewportWidth = 480;
  parser.viewportHeight = 64;
  const char* cells[] = {
      "This is one very long sentence. It is part of the first cell of this table. It is quite nice.",
      "This is the second cell. It will probably overflow into the next page. Lorem ipsum dolor sit amet, consetetur "
      "sadipscing elitr, sed diam nonumy eirmod tempor invidunt ut labore et dolore magna aliquyam erat, sed diam "
      "voluptua. At vero eos et accusam et justo duo dolores et ea rebum. Stet clita kasd gubergren, no sea takimata "
      "sanctus est Lorem ipsum dolor sit amet. Lorem ipsum dolor sit amet, consetetur sadipscing elitr, sed diam "
      "nonumy eirmod tempor invidunt ut labore et dolore magna aliquyam erat, sed diam voluptua. At vero eos et "
      "accusam et justo duo dolores et ea rebum. Stet clita kasd gubergren, no sea takimata sanctus est Lorem ipsum "
      "dolor sit amet."};
  std::multiset<std::string> expected[2];
  open("table", nullptr);
  parser.tableColumnWidths = {160, 320};
  parser.tableColumnCount = 2;
  open("tr", nullptr);
  for (size_t column = 0; column < 2; ++column) {
    open("td", "padding: 3px 6px");
    std::istringstream input(cells[column]);
    for (std::string word; input >> word;) expected[column].insert(word);
    // XML can split a cell across arbitrary character-data callbacks.
    const std::string content(cells[column]);
    for (size_t offset = 0; offset < content.size(); offset += 17) text(content.substr(offset, 17));
    close("td");
    EXPECT_FALSE(parser.tableRowStacked);
  }
  close("tr");
  close("table");
  const auto lines = elementsWithTag(TAG_PageLine);
  ASSERT_GT(pages.size(), 1u);
  std::multiset<std::string> actual[2];
  for (const auto* element : lines) {
    const auto& line = *static_cast<const PageLine*>(element);
    const size_t column = line.xPos == 6 ? 0 : 1;
    EXPECT_EQ(line.xPos, column == 0 ? 6 : 166);
    EXPECT_LE(line.yPos + renderer.getLineHeight(0), parser.viewportHeight);
    for (uint16_t i = 0; i < line.getBlock()->wordCount(); ++i) {
      actual[column].insert(line.getBlock()->wordText(i));
    }
  }
  EXPECT_EQ(actual[0], expected[0]);
  EXPECT_EQ(actual[1], expected[1]);
}

TEST_F(ChapterTableBorderTest, RowBudgetFallsBackAndResetsForTheNextRow) {
  open("table", nullptr);
  open("tr", nullptr);
  for (int column = 0; column < 2; ++column) {
    open("td", nullptr);
    text(words(100));
    close("td");
  }
  EXPECT_TRUE(parser.tableRowStacked);
  close("tr");
  open("tr", nullptr);
  EXPECT_EQ(parser.tableRowTextBytes, 0u);
  for (int column = 0; column < 2; ++column) {
    open("td", nullptr);
    text(words(70));
    close("td");
    EXPECT_FALSE(parser.tableRowStacked);
  }
  close("tr");
  close("table");
  size_t count = 0;
  for (const auto* element : elementsWithTag(TAG_PageLine)) {
    count += static_cast<const PageLine*>(element)->getBlock()->wordCount();
  }
  EXPECT_EQ(count, 340u);
}

class ChapterWidowOrphanTest : public ChapterBlockDecorationTest {
 protected:
  void SetUp() override {
    ChapterBlockDecorationTest::SetUp();
    parser.viewportHeight = 5 * 16;  // five fixture lines per page
  }
  void paragraph(int wordCount) {
    open("p", nullptr);
    text(words(wordCount));
    close("p");
  }
  size_t linesOnPage(size_t index) {
    size_t count = 0;
    for (const auto& element : pages[index]->elements) count += element->getTag() == TAG_PageLine;
    return count;
  }
};

TEST_F(ChapterWidowOrphanTest, LoneFirstLineMovesToNextPage) {
  paragraph(13 * 4);  // four full lines
  paragraph(13 * 3);  // its first line would be alone at the page bottom
  ASSERT_GE(pages.size(), 1u);
  EXPECT_EQ(linesOnPage(0), 4u);
}

TEST_F(ChapterWidowOrphanTest, LoneLastLineTakesALineAlong) {
  paragraph(13 * 5 + 3);  // six lines: the sixth would open page two alone
  paragraph(13);
  ASSERT_GE(pages.size(), 1u);
  EXPECT_EQ(linesOnPage(0), 4u);
  const auto& firstOnNext = *parser.currentPage->elements.front();
  EXPECT_EQ(firstOnNext.yPos, 0);
}

TEST_F(ChapterWidowOrphanTest, HeadingMovesWithTheParagraphAfterIt) {
  paragraph(13 * 4);  // four lines
  open("h2", nullptr);
  text("Heading");
  close("h2");        // fits as the page's fifth line
  paragraph(13 * 3);  // cannot start on this page
  ASSERT_GE(pages.size(), 1u);
  EXPECT_EQ(linesOnPage(0), 4u);
  const auto& first = *static_cast<const PageLine&>(*parser.currentPage->elements.front()).getBlock();
  EXPECT_STREQ(first.wordText(0), "Heading");
}

TEST_F(ChapterWidowOrphanTest, HeadingStaysWhenFollowedOnItsPage) {
  paragraph(13 * 2);
  open("h2", nullptr);
  text("Heading");
  close("h2");
  paragraph(13 * 4);  // two of its four lines fit below the heading, two follow
  ASSERT_GE(pages.size(), 1u);
  EXPECT_EQ(linesOnPage(0), 5u);
}

TEST_F(ChapterWidowOrphanTest, TwoLineParagraphMovesWhole) {
  paragraph(13 * 4);
  paragraph(13 * 2);  // one line fits: moving just one would orphan, so both move
  ASSERT_GE(pages.size(), 1u);
  EXPECT_EQ(linesOnPage(0), 4u);
}

TEST_F(ChapterHtmlSlimParserTest, PassesIndentSettingsToNewTextBlock) {
  for (bool extraSpacing : {false, true}) {
    parser.extraParagraphSpacing = extraSpacing;
    parser.currentTextBlock.reset();
    parser.setParagraphIndentSpaces(5);
    parser.startNewTextBlock(BlockStyle());
    ASSERT_NE(parser.currentTextBlock, nullptr);
    EXPECT_EQ(parser.currentTextBlock->paragraphIndentSpaces, 5);
  }
}

}  // namespace

TEST(ParagraphIndentation, OverridesNonnegativeCssAndPreservesHangingIndent) {
  GfxRenderer renderer;
  for (int cssIndent : {-6, 0, 13}) {
    for (uint8_t spaces : {0, 1, 2, 5}) {
      BlockStyle style;
      style.alignment = CssTextAlign::Left;
      style.textIndentDefined = true;
      style.textIndent = cssIndent;
      ParsedText text(false, false, style, spaces);
      text.addWord("word", EpdFontFamily::REGULAR);
      bool sawLine = false;
      text.layoutAndExtractLines(renderer, 0, 200, [&](std::unique_ptr<TextBlock> line, auto) {
        sawLine = true;
        EXPECT_EQ(line->wordXpos(0), cssIndent < 0 ? cssIndent : 4 * spaces);
      });
      EXPECT_TRUE(sawLine);
    }
  }
  for (uint8_t spaces : {0, 2}) {
    BlockStyle style;
    style.alignment = CssTextAlign::Left;
    ParsedText text(false, false, style, spaces);
    text.addWord("word", EpdFontFamily::REGULAR);
    text.layoutAndExtractLines(
        renderer, 0, 200, [&](std::unique_ptr<TextBlock> line, auto) { EXPECT_EQ(line->wordXpos(0), 4 * spaces); });
  }
}

TEST(ParagraphIndentation, PreservesAlignmentEligibilityAndScaledSpaceRounding) {
  GfxRenderer renderer;
  for (const auto alignment : {CssTextAlign::Left, CssTextAlign::Center}) {
    for (uint8_t spaces : {0, 2}) {
      BlockStyle style;
      style.alignment = alignment;
      style.textIndentDefined = true;
      style.textIndent = 0;
      ParsedText text(false, false, style, spaces);
      text.addWord("word", EpdFontFamily::REGULAR);
      text.layoutAndExtractLines(renderer, 0, 200, [&](std::unique_ptr<TextBlock> line, auto) {
        if (alignment == CssTextAlign::Left)
          EXPECT_EQ(line->wordXpos(0), 4 * spaces);
        else
          EXPECT_EQ(line->wordXpos(0), 84);
      });
    }
  }
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  ParsedText text(false, false, style, 2);
  text.addWord("word", EpdFontFamily::REGULAR);
  text.layoutAndExtractLines(
      renderer, 0, 200, [&](std::unique_ptr<TextBlock> line, auto) { EXPECT_EQ(line->wordXpos(0), 6); }, true, 0, 75);
}

TEST(ParagraphIndentation, ReducesOnlyFirstLineAvailableWidth) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  for (uint8_t spaces : {1, 2, 5}) {
    ParsedText text(false, false, style, spaces);
    text.addWord("ab", EpdFontFamily::REGULAR);
    text.addWord("cd", EpdFontFamily::REGULAR);
    unsigned lines = 0;
    text.layoutAndExtractLines(renderer, 0, 40, [&](std::unique_ptr<TextBlock>, auto) { ++lines; });
    EXPECT_EQ(lines, spaces == 1 ? 1u : 2u);
  }
}

TEST(ParagraphIndentation, PreviewKeyTracksOffAndWidths) {
  textsettings::PreviewKey off;
  EXPECT_EQ(off.paragraphIndentSpaces, 2);
  off.paragraphIndentSpaces = 0;
  auto on = off;
  on.paragraphIndentSpaces = 5;
  EXPECT_NE(off, on);
  on.paragraphIndentSpaces = 2;
  EXPECT_NE(off, on);
}

TEST(ParagraphIndentation, MigratesLegacySettingsAndClampsWidths) {
  EXPECT_EQ(migrateParagraphIndentSpaces(false, 0, true), 0);
  EXPECT_EQ(migrateParagraphIndentSpaces(false, 0, false), 2);
  EXPECT_EQ(migrateParagraphIndentSpaces(true, 0, false), 0);
  EXPECT_EQ(migrateParagraphIndentSpaces(true, 2, true), 2);
  EXPECT_EQ(migrateParagraphIndentSpaces(true, 5, false), 5);
  EXPECT_EQ(migrateParagraphIndentSpaces(true, -1, false), 0);
  EXPECT_EQ(migrateParagraphIndentSpaces(true, 300, false), 5);
}

TEST(TextSpacingLayout, TrackingSeparatesCjkTokensAndScalesWordSpaces) {
  GfxRenderer renderer;
  for (bool hyphenation : {false, true}) {
    BlockStyle style;
    style.alignment = CssTextAlign::Left;
    style.textIndentDefined = true;
    ParsedText text(hyphenation, false, style, 0);
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
    ParsedText text(false, false, style, 0);
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
  ParsedText text(false, false, style);
  text.addWord("一二三", EpdFontFamily::REGULAR);
  text.addWord("四五", EpdFontFamily::REGULAR);
  const auto path = (std::filesystem::temp_directory_path() / "crosspoint-text-spacing.bin").string();
  unsigned lines = 0;
  text.layoutAndExtractLines(
      renderer, 0, 200,
      [&](std::unique_ptr<TextBlock> line, auto) {
        ++lines;
        Page page;
        page.elements.push_back(std::make_unique<PageLine>(std::move(line), 4, 12));
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
    ParsedText text(false, false, style, 0);
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
  ParsedText text(false, false, style, 0);
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
  ParsedText text(false, false, style);
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

TEST(TextDecorationPosition, StrikeUsesGlyphBodyAndFollowsScriptScaling) {
  GfxRenderer renderer;
  constexpr int top = 20;
  for (const auto script : {EpdFontFamily::REGULAR, EpdFontFamily::SUP, EpdFontFamily::SUB}) {
    ParsedText text(false, false, BlockStyle{}, 0);
    text.addWord("example", static_cast<EpdFontFamily::Style>(EpdFontFamily::STRIKETHROUGH | script));
    text.layoutAndExtractLines(renderer, 0, 200, [&](std::unique_ptr<TextBlock> line, uint32_t) {
      renderer.drawnLineCount = 0;
      line->render(renderer, 0, 0, top);
      ASSERT_EQ(renderer.drawnLineCount, 1);
      // Fixture baseline is top + 12; glyph body is 10 pixels, halved for scripts.
      const int expected = script == EpdFontFamily::SUP ? 26 : script == EpdFontFamily::SUB ? 33 : 27;
      EXPECT_EQ(renderer.lastLineY, expected);
    });
  }
}

TEST(TextDecorationPosition, UnderlineStaysBelowBaseline) {
  GfxRenderer renderer;
  ParsedText text(false, false, BlockStyle{}, 0);
  text.addWord("example", EpdFontFamily::UNDERLINE);
  text.layoutAndExtractLines(renderer, 0, 200, [&](std::unique_ptr<TextBlock> line, uint32_t) {
    line->render(renderer, 0, 0, 20);
    EXPECT_EQ(renderer.lastLineY, 34);
  });
}
