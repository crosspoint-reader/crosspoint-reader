#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "ChapterXPathResolver.h"
#include "ProgressMapper.h"

namespace {

constexpr char NESTED_CHAPTER[] =
    "<html><head><title>ignored</title></head><body><div><p>alpha <em>beta</em> omega</p></div></body></html>";

std::shared_ptr<Epub> makeEpub(const std::string& chapter, const size_t chunkSize = 0) {
  return std::make_shared<Epub>(std::vector<std::string>{chapter}, chunkSize);
}

CrossPointPosition positionAt(const uint32_t offset) {
  CrossPointPosition position{};
  position.spineIndex = 0;
  position.pageNumber = 1;
  position.totalPages = 10;
  position.paragraphIndex = 1;
  position.hasParagraphIndex = true;
  position.visibleTextOffset = offset;
  position.hasVisibleTextOffset = true;
  return position;
}

TEST(KOReaderPosition, ExportsExactInlineOffsetBeforeParagraphFallback) {
  const auto saved = ProgressMapper::toSavedProgress(makeEpub(NESTED_CHAPTER), positionAt(8));
  EXPECT_EQ(saved.xpath, "/body/DocFragment[1]/body/div[1]/p[1]/em[1]/text()[1].2");
}

TEST(KOReaderPosition, PreservesZeroOffsetOfTheSecondDirectTextNode) {
  const auto saved = ProgressMapper::toSavedProgress(makeEpub(NESTED_CHAPTER), positionAt(10));
  EXPECT_EQ(saved.xpath, "/body/DocFragment[1]/body/div[1]/p[1]/text()[2].0");
}

TEST(KOReaderPosition, ProducesTheSameAnchorAcrossByteChunks) {
  for (size_t chunkSize = 1; chunkSize <= 17; ++chunkSize) {
    SCOPED_TRACE(chunkSize);
    const auto epub = makeEpub(NESTED_CHAPTER, chunkSize);
    EXPECT_EQ(ProgressMapper::toSavedProgress(epub, positionAt(8)).xpath,
              "/body/DocFragment[1]/body/div[1]/p[1]/em[1]/text()[1].2");
    EXPECT_EQ(ProgressMapper::toSavedProgress(epub, positionAt(10)).xpath,
              "/body/DocFragment[1]/body/div[1]/p[1]/text()[2].0");
  }
}

TEST(KOReaderPosition, CountsUnicodeCodepointsAndSkipsNonVisibleElements) {
  const auto epub = makeEpub("<html><body>é<script>hidden</script>猫<p>e&#x301;😀x</p></body></html>", 1);
  EXPECT_EQ(ProgressMapper::toSavedProgress(epub, positionAt(1)).xpath, "/body/DocFragment[1]/body/text()[2].0");
  EXPECT_EQ(ProgressMapper::toSavedProgress(epub, positionAt(5)).xpath, "/body/DocFragment[1]/body/p[1]/text()[1].3");
}

TEST(KOReaderPosition, NormalizesLineEndingsAndCountsBodyWhitespace) {
  const std::string chapter = "<html><body>\r\n<p>a\r\nb\t c</p>\r</body></html>";
  for (size_t chunkSize = 1; chunkSize <= 9; ++chunkSize) {
    SCOPED_TRACE(chunkSize);
    const auto epub = makeEpub(chapter, chunkSize);
    EXPECT_EQ(ProgressMapper::toSavedProgress(epub, positionAt(0)).xpath, "/body/DocFragment[1]/body/text()[1].0");
    EXPECT_EQ(ProgressMapper::toSavedProgress(epub, positionAt(3)).xpath, "/body/DocFragment[1]/body/p[1]/text()[1].2");
    EXPECT_EQ(ProgressMapper::toSavedProgress(epub, positionAt(7)).xpath, "/body/DocFragment[1]/body/text()[2].0");
  }
}

TEST(KOReaderPosition, CountsBuiltinAndNumericEntitiesAsDecodedCodepoints) {
  const std::string chapter = "<html><body><p>a&amp;&#x732B;&#160;&#128512;<em>x</em>z</p></body></html>";
  for (size_t chunkSize = 1; chunkSize <= 13; ++chunkSize) {
    SCOPED_TRACE(chunkSize);
    const auto epub = makeEpub(chapter, chunkSize);
    EXPECT_EQ(ProgressMapper::toSavedProgress(epub, positionAt(4)).xpath, "/body/DocFragment[1]/body/p[1]/text()[1].4");
    EXPECT_EQ(ProgressMapper::toSavedProgress(epub, positionAt(5)).xpath,
              "/body/DocFragment[1]/body/p[1]/em[1]/text()[1].0");
    EXPECT_EQ(ProgressMapper::toSavedProgress(epub, positionAt(6)).xpath, "/body/DocFragment[1]/body/p[1]/text()[2].0");
  }
}

TEST(KOReaderPosition, ResolvesHtmlEntitiesWithFirmwareExpatConfiguration) {
  const auto epub = makeEpub("<!DOCTYPE html [<!ENTITY nbsp '&#160;'>]><html><body><p>a&nbsp;b</p></body></html>", 1);
  EXPECT_EQ(ProgressMapper::toSavedProgress(epub, positionAt(2)).xpath, "/body/DocFragment[1]/body/p[1]/text()[1].2");
}

TEST(KOReaderPosition, ResolvesBodyDivAndListTextWithoutParagraphs) {
  const auto epub = makeEpub("<html><body>a<div>b</div><ul><li>c</li><li>d</li></ul>e</body></html>", 1);
  EXPECT_EQ(ProgressMapper::toSavedProgress(epub, positionAt(1)).xpath, "/body/DocFragment[1]/body/div[1]/text()[1].0");
  EXPECT_EQ(ProgressMapper::toSavedProgress(epub, positionAt(3)).xpath,
            "/body/DocFragment[1]/body/ul[1]/li[2]/text()[1].0");
  EXPECT_EQ(ProgressMapper::toSavedProgress(epub, positionAt(4)).xpath, "/body/DocFragment[1]/body/text()[2].0");
}

TEST(KOReaderPosition, ResolvesDocumentEndToTheLastTextNode) {
  const auto epub = makeEpub("<html><body><p>abc</p></body></html>", 1);
  EXPECT_EQ(ProgressMapper::toSavedProgress(epub, positionAt(3)).xpath, "/body/DocFragment[1]/body/p[1]/text()[1].3");
}

TEST(KOReaderPosition, KeepsLegacyParagraphFallbackWhenExactOffsetIsUnavailable) {
  const auto epub = makeEpub(NESTED_CHAPTER);
  auto position = positionAt(8);
  position.hasVisibleTextOffset = false;
  EXPECT_EQ(ProgressMapper::toSavedProgress(epub, position).xpath, "/body/DocFragment[1]/body/div[1]/p[1]");
  position.hasVisibleTextOffset = true;
  position.visibleTextOffset = 10000;
  EXPECT_EQ(ProgressMapper::toSavedProgress(epub, position).xpath, "/body/DocFragment[1]/body/div[1]/p[1]");
}

TEST(KOReaderPosition, KeepsProgressFallbackWhenNeitherExactOffsetNorParagraphIsAvailable) {
  const auto epub = makeEpub("<html><body><p>abcdefghij</p></body></html>");
  auto position = positionAt(1000);
  position.pageNumber = 1;
  position.totalPages = 3;
  position.hasParagraphIndex = false;
  EXPECT_EQ(ProgressMapper::toSavedProgress(epub, position).xpath, "/body/DocFragment[1]/body/p[1]/text()[1].5");
  position.hasVisibleTextOffset = false;
  EXPECT_EQ(ProgressMapper::toSavedProgress(epub, position).xpath, "/body/DocFragment[1]/body/p[1]/text()[1].5");
}

TEST(KOReaderPosition, FailedXmlMappingRetainsLegacyFallback) {
  const auto epub = makeEpub("<html><body><p>&unknown;broken</p></body></html>");
  auto position = positionAt(1000);
  const auto saved = ProgressMapper::toSavedProgress(epub, position);
  position.hasVisibleTextOffset = false;
  const auto fallback = ProgressMapper::toSavedProgress(epub, position);
  EXPECT_FALSE(saved.xpath.empty());
  EXPECT_EQ(saved.xpath, fallback.xpath);
  EXPECT_FLOAT_EQ(saved.percentage, fallback.percentage);
}

TEST(KOReaderPosition, ExactAnchorKeepsTheExistingGlobalPercentage) {
  const auto epub = std::make_shared<Epub>(std::vector<std::string>{NESTED_CHAPTER, NESTED_CHAPTER});
  auto position = positionAt(8);
  position.spineIndex = 1;
  const auto exact = ProgressMapper::toSavedProgress(epub, position);
  position.hasVisibleTextOffset = false;
  const auto fallback = ProgressMapper::toSavedProgress(epub, position);
  EXPECT_FLOAT_EQ(exact.percentage, fallback.percentage);
  EXPECT_EQ(exact.xpath, "/body/DocFragment[2]/body/div[1]/p[1]/em[1]/text()[1].2");
}

TEST(KOReaderPosition, ExportedAnchorsRoundTripThroughTheExistingImporter) {
  const auto epub = makeEpub(NESTED_CHAPTER, 1);
  GfxRenderer renderer;
  for (uint32_t offset = 0; offset <= 16; ++offset) {
    SCOPED_TRACE(offset);
    const auto saved = ProgressMapper::toSavedProgress(epub, positionAt(offset));
    const auto restored = ProgressMapper::toCrossPoint(epub, saved, renderer);
    ASSERT_TRUE(restored.hasVisibleTextOffset);
    EXPECT_EQ(restored.spineIndex, 0);
    EXPECT_EQ(restored.visibleTextOffset, offset);
  }
}

TEST(KOReaderPosition, ExactResolverRejectsInvalidSpinesAndOutOfRangeOffsets) {
  const auto epub = makeEpub("<html><body><p>abc</p></body></html>");
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(nullptr, 0, 0).empty());
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, -1, 0).empty());
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 1, 0).empty());
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 4).empty());
}

TEST(KOReaderPosition, ExactResolverLeavesEmptyAndMalformedChaptersToFallbacks) {
  const auto empty = makeEpub("<html><body><img src='cover.png'/></body></html>");
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(empty, 0, 0).empty());
  const auto malformed = makeEpub("<html><body><p>&unknown;broken</p></body></html>");
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(malformed, 0, 1).empty());
}

TEST(KOReaderPosition, ExcessiveNestingLeavesTheLegacyFallbackAvailable) {
  std::string chapter = "<html><body><script>";
  for (int depth = 0; depth < 260; ++depth) chapter += "<span>";
  chapter += "hidden";
  for (int depth = 0; depth < 260; ++depth) chapter += "</span>";
  chapter += "</script><p>visible</p></body></html>";
  const auto epub = makeEpub(chapter, 1);
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0).empty());
  auto position = positionAt(0);
  const auto saved = ProgressMapper::toSavedProgress(epub, position);
  position.hasVisibleTextOffset = false;
  EXPECT_EQ(saved.xpath, ProgressMapper::toSavedProgress(epub, position).xpath);
}

TEST(KOReaderPosition, UnsupportedBodyXmlUsesLegacyFallbackAfterTheMarkup) {
  for (const std::string markup : {"<!-- comment -->", "<?target data?>", "<![CDATA[x]]>"}) {
    SCOPED_TRACE(markup);
    const auto epub = makeEpub("<html><body><p>a" + markup + "b</p></body></html>", 1);
    EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 1).empty());
    auto position = positionAt(1);
    const auto saved = ProgressMapper::toSavedProgress(epub, position);
    position.hasVisibleTextOffset = false;
    EXPECT_EQ(saved.xpath, ProgressMapper::toSavedProgress(epub, position).xpath);
  }
}

TEST(KOReaderPosition, UnsupportedMarkupDoesNotDiscardAnEarlierExactAnchor) {
  const auto epub = makeEpub("<html><head><!-- metadata --></head><body><p>ab<!-- later -->cd</p></body></html>", 1);
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 1),
            "/body/DocFragment[1]/body/p[1]/text()[1].1");
}

TEST(KOReaderPosition, ImageBetweenDirectTextNodesPreservesExactRoundTrips) {
  const auto epub = makeEpub("<html><body><p>ab<img src='images/cover.png'/>cd</p></body></html>", 1);
  EXPECT_EQ(ProgressMapper::toSavedProgress(epub, positionAt(2)).xpath, "/body/DocFragment[1]/body/p[1]/text()[2].0");
  GfxRenderer renderer;
  for (uint32_t offset = 0; offset <= 4; ++offset) {
    SCOPED_TRACE(offset);
    const auto saved = ProgressMapper::toSavedProgress(epub, positionAt(offset));
    const auto restored = ProgressMapper::toCrossPoint(epub, saved, renderer);
    ASSERT_TRUE(restored.hasVisibleTextOffset);
    EXPECT_EQ(restored.visibleTextOffset, offset);
  }
}

TEST(KOReaderPosition, DirectTextNodesRoundTripAfterLeadingConsecutiveAndHiddenChildren) {
  struct Fixture {
    const char* body;
    uint32_t visibleLength;
  };
  const Fixture fixtures[] = {
      {"<p><em>alpha</em> beta</p>", 10},
      {"<p>ab<em>x</em><strong>y</strong>cd</p>", 6},
      {"a<div>b</div><ul><li>c</li><li>d</li></ul>e", 5},
      {"<p>ab<script>hidden</script>cd</p>", 4},
      {"<p><img/><img/>ab<img/><img/>cd</p>", 4},
      {"<div><p><em>é猫</em><strong>😀</strong>e&#x301;z</p></div>", 6},
      {"<p>a<style>hidden</style><script>hidden</script>é&#x732B;😀</p>", 4},
      {"<p><em>ab</em></p>cd", 4},
  };
  GfxRenderer renderer;
  for (const auto& fixture : fixtures) {
    SCOPED_TRACE(fixture.body);
    for (size_t chunkSize = 1; chunkSize <= 17; ++chunkSize) {
      SCOPED_TRACE(chunkSize);
      const auto epub = makeEpub(std::string("<html><body>") + fixture.body + "</body></html>", chunkSize);
      for (uint32_t offset = 0; offset <= fixture.visibleLength; ++offset) {
        SCOPED_TRACE(offset);
        const auto saved = ProgressMapper::toSavedProgress(epub, positionAt(offset));
        SCOPED_TRACE(saved.xpath);
        const auto restored = ProgressMapper::toCrossPoint(epub, saved, renderer);
        ASSERT_TRUE(restored.hasVisibleTextOffset);
        EXPECT_EQ(restored.visibleTextOffset, offset);
      }
    }
  }
}

TEST(KOReaderPosition, ElementAnchorsRemainAtElementStartBeforeLeadingInlineText) {
  const auto epub =
      makeEpub("<html><body>xy<p>first</p><div><p><em>abc</em>def</p></div><img src='x.png'/></body></html>", 1);
  GfxRenderer renderer;
  for (const std::string anchor : {"/p[2]", "/div[1]/p[1].0"}) {
    SCOPED_TRACE(anchor);
    const SavedProgressPosition saved{"/body/DocFragment[1]/body" + anchor, 0.5f};
    const auto restored = ProgressMapper::toCrossPoint(epub, saved, renderer);
    ASSERT_TRUE(restored.hasVisibleTextOffset);
    EXPECT_EQ(restored.visibleTextOffset, 7u);
  }
  const SavedProgressPosition image{"/body/DocFragment[1]/body/img[1].0", 0.5f};
  EXPECT_EQ(ProgressMapper::toCrossPoint(epub, image, renderer).visibleTextOffset, 13u);
  const SavedProgressPosition text{"/body/DocFragment[1]/body/div[1]/p[1]/text().0", 0.5f};
  const auto restoredText = ProgressMapper::toCrossPoint(epub, text, renderer);
  ASSERT_TRUE(restoredText.hasVisibleTextOffset);
  EXPECT_EQ(restoredText.visibleTextOffset, 10u);
}

}  // namespace
