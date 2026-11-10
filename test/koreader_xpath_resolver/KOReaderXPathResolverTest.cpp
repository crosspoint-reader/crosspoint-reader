#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ChapterXPathResolver.h"

namespace {
std::shared_ptr<Epub> epubWith(std::string xhtml) {
  std::vector<std::string> spine;
  spine.push_back(std::move(xhtml));
  return std::make_shared<Epub>(std::move(spine));
}

constexpr char kNestedFixture[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><div><section><p>Alpha bravo</p><p>Second <em>nested</em> tail</p></section></div></body></html>)";

constexpr char kNonVisibleInlineFixture[] =
    R"(<html><body><p><RP><span>hidden</span></RP>Visible text</p></body></html>)";

constexpr char kCommentBoundaryFixture[] = R"(<html><body><p>before<!--comment-->after</p></body></html>)";
constexpr char kProcessingInstructionBoundaryFixture[] = R"(<html><body><p>before<?marker?>after</p></body></html>)";
constexpr char kCdataBoundaryFixture[] = R"(<html><body><p>before<![CDATA[middle]]>after</p></body></html>)";
constexpr char kHiddenCdataFixture[] = R"(<html><body><p>before<rp><![CDATA[hidden]]></rp>after</p></body></html>)";

// Calibre output from Kindle/MOBI sources may carry body text in <div>/<span> with no <p>
// beyond an empty filepos anchor, when the source MOBI was authored that way (it is not
// universal: a Project Gutenberg MOBI converts back to <p> paragraphs).
// Visible offsets below follow the layout parser: every body codepoint counts, including
// formatting whitespace between blocks.
//   0 "\n"   1 "\n"   2 "3"   3 " "   4 "\n"   5-13 "She drove"   14-26 " back to D.C."
//   27 "\n"  28-33 "\u201cWho?\u201d"   34 "\n"
constexpr char kKindleDivSpanFixture[] =
    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body class=\"calibre\">\n"
    "<p id=\"filepos1\" class=\"calibre1\"></p>\n"
    "<div class=\"calibre8\"><span class=\"calibre9\">3</span></div><div class=\"calibre10\"> </div>\n"
    "<div class=\"calibre12\"><span class=\"calibre6\"><span class=\"bold\">She drove</span> back to "
    "D.C.</span></div>\n"
    "<div class=\"calibre19\"><span class=\"calibre6\">\xE2\x80\x9CWho?\xE2\x80\x9D</span></div>\n"
    "</body></html>";
}  // namespace

TEST(KOReaderXPathResolver, ResolvesExactOffsetWithFullAncestry) {
  const auto epub = epubWith(kNestedFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 6),
            "/body/DocFragment[1]/body/div[1]/section[1]/p[1]/text()[1].6");
}

TEST(KOReaderXPathResolver, PreservesNestedInlineTextNode) {
  const auto epub = epubWith(kNestedFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 26),
            "/body/DocFragment[1]/body/div[1]/section[1]/p[2]/text()[2].2");
}

TEST(KOReaderXPathResolver, EmitsDetailedAnchorForOffsetZero) {
  const auto epub = epubWith(kNestedFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0),
            "/body/DocFragment[1]/body/div[1]/section[1]/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, IgnoresNestedNonVisibleInlineText) {
  const auto epub = epubWith(kNonVisibleInlineFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0),
            "/body/DocFragment[1]/body/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, ResolvesProgressAfterNestedNonVisibleInlineText) {
  const auto epub = epubWith(kNonVisibleInlineFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 1.0f), "/body/DocFragment[1]/body/p[1]/text()[1].12");
}

TEST(KOReaderXPathResolver, CountsUtf8CodepointsInsteadOfBytes) {
  const auto epub = epubWith(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><html><body><p>A\xC3\xA9\xE4\xB8\xAD"
      "B</p></body></html>");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 3),
            "/body/DocFragment[1]/body/p[1]/text()[1].3");
}

TEST(KOReaderXPathResolver, SplitsTextNodesAroundComments) {
  const auto epub = epubWith(kCommentBoundaryFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 6),
            "/body/DocFragment[1]/body/p[1]/text()[2].0");
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 11).empty());
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
}

TEST(KOReaderXPathResolver, SplitsTextNodesAroundProcessingInstructions) {
  const auto epub = epubWith(kProcessingInstructionBoundaryFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
}

TEST(KOReaderXPathResolver, SplitsTextNodesAroundCdata) {
  const auto epub = epubWith(kCdataBoundaryFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 13),
            "/body/DocFragment[1]/body/p[1]/text()[3].1");
}

TEST(KOReaderXPathResolver, DoesNotCreateNodesBeforeFirstComment) {
  const auto epub = epubWith(R"(<html><body><p><!--comment-->text</p></body></html>)");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0),
            "/body/DocFragment[1]/body/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, DoesNotCreateNodesBeforeFirstProcessingInstruction) {
  const auto epub = epubWith(R"(<html><body><p><?marker?>text</p></body></html>)");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0),
            "/body/DocFragment[1]/body/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, DoesNotCreateNodesBeforeFirstCdata) {
  const auto epub = epubWith(R"(<html><body><p><![CDATA[text]]></p></body></html>)");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0),
            "/body/DocFragment[1]/body/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, CountsVisibleCdataAndIgnoresHiddenCdata) {
  const auto visible = epubWith(kCdataBoundaryFixture);
  const auto hidden = epubWith(kHiddenCdataFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(visible, 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(hidden, 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
}

TEST(KOReaderXPathResolver, ReturnsEmptyForUnusableContent) {
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(""), 0, 0).empty());
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith("<html><body><p>broken"), 0, 100).empty());
}

TEST(KOReaderXPathResolver, KeepsParagraphOnlyResolutionUnchanged) {
  const auto epub = epubWith(kNestedFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForParagraph(epub, 0, 2),
            "/body/DocFragment[1]/body/div[1]/section[1]/p[2]");
}

TEST(KOReaderXPathResolver, ResolvesTextOutsideParagraphElements) {
  const auto epub = epubWith(kKindleDivSpanFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 2),
            "/body/DocFragment[1]/body/div[1]/span[1]/text()[1].0");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 5),
            "/body/DocFragment[1]/body/div[3]/span[1]/span[1]/text()[1].0");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 20),
            "/body/DocFragment[1]/body/div[3]/span[1]/text()[1].6");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 28),
            "/body/DocFragment[1]/body/div[4]/span[1]/text()[1].0");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(
                epubWith("<html><body><div>not a paragraph or list item</div></body></html>"), 0, 0),
            "/body/DocFragment[1]/body/div[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, ResolvesProgressOutsideParagraphElements) {
  const auto epub = epubWith(kKindleDivSpanFixture);

  // Progress spans the text up to the last non-whitespace codepoint (34), so 1.0 lands at the
  // end of the final text node rather than in the trailing formatting newline.
  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 1.0f),
            "/body/DocFragment[1]/body/div[4]/span[1]/text()[1].6");
  // ceil(0.5 * 34) = 17 -> " back to D.C." starts at 14 -> offset 3 within the node.
  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 0.5f),
            "/body/DocFragment[1]/body/div[3]/span[1]/text()[1].3");
}

TEST(KOReaderXPathResolver, SkipsWhitespaceOnlyTextWhenResolvingOffsets) {
  const auto epub = epubWith(kKindleDivSpanFixture);

  // Formatting whitespace between blocks is never a usable anchor: resolve to the next text.
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 1),
            "/body/DocFragment[1]/body/div[1]/span[1]/text()[1].0");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 3),
            "/body/DocFragment[1]/body/div[3]/span[1]/span[1]/text()[1].0");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 4),
            "/body/DocFragment[1]/body/div[3]/span[1]/span[1]/text()[1].0");
  // Trailing whitespace with no text after it cannot be resolved.
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 34).empty());
}

TEST(KOReaderXPathResolver, ResolvesTextDirectlyInsideBody) {
  const auto bare = epubWith("<html><body>Hello world</body></html>");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(bare, 0, 6), "/body/DocFragment[1]/body/text()[1].6");

  // "\nHello" is one text run after the <p>: text()[1] at offset 1.
  const auto afterBlock = epubWith("<html><body><p>a</p>\nHello</body></html>");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(afterBlock, 0, 2),
            "/body/DocFragment[1]/body/text()[1].1");
}

TEST(KOReaderXPathResolver, ProgressEndExcludesTrailingWhitespaceInsideRun) {
  const auto epub = epubWith("<html><body><p>Text   </p></body></html>");
  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 1.0f), "/body/DocFragment[1]/body/p[1]/text()[1].4");
}
