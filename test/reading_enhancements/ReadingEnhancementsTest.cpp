#include <Epub/EpubSearch.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

bool isValidUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const uint8_t b = static_cast<uint8_t>(s[i]);
    size_t len = 0;
    if ((b & 0x80) == 0) {
      len = 1;
    } else if ((b & 0xE0) == 0xC0) {
      len = 2;
    } else if ((b & 0xF0) == 0xE0) {
      len = 3;
    } else if ((b & 0xF8) == 0xF0) {
      len = 4;
    } else {
      return false;
    }
    if (i + len > s.size()) return false;
    for (size_t k = 1; k < len; ++k) {
      if ((static_cast<uint8_t>(s[i + k]) & 0xC0) != 0x80) return false;
    }
    i += len;
  }
  return true;
}

}  // namespace

TEST(InBookSearchTest, StripsTagsAndMatchesText) {
  const std::string html = "<p>The quick <b>brown</b> fox jumps over the lazy dog.</p>";
  std::vector<EpubSearchResult> matches;
  ASSERT_TRUE(EpubSearch::searchHtml(html, "brown", 0, "Ch 1", matches));

  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches[0].match, "brown");
  EXPECT_EQ(matches[0].visibleTextOffset, 10u);
  EXPECT_EQ(matches[0].preContext, "The quick ");
  EXPECT_EQ(matches[0].postContext, " fox jumps over the lazy");
}

TEST(InBookSearchTest, CaseInsensitiveSearch) {
  const std::string html = "<h1>CHAPTER ONE</h1><p>Captain Nemo looked out the window.</p>";

  std::vector<EpubSearchResult> matches;
  ASSERT_TRUE(EpubSearch::searchHtml(html, "nemo", 0, "Ch 1", matches));
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches[0].match, "Nemo");

  std::vector<EpubSearchResult> matches2;
  ASSERT_TRUE(EpubSearch::searchHtml(html, "CAPTAIN", 0, "Ch 1", matches2));
  ASSERT_EQ(matches2.size(), 1u);
  EXPECT_EQ(matches2[0].match, "Captain");
}

TEST(InBookSearchTest, DoesNotMatchInsideHtmlTags) {
  const std::string html = "<div class=\"important-class\" id=\"chapter_box\">Normal text here.</div>";

  std::vector<EpubSearchResult> matches;
  EXPECT_FALSE(EpubSearch::searchHtml(html, "important", 0, "Ch 1", matches));
  EXPECT_EQ(matches.size(), 0u);

  std::vector<EpubSearchResult> matches2;
  ASSERT_TRUE(EpubSearch::searchHtml(html, "Normal", 0, "Ch 1", matches2));
  EXPECT_EQ(matches2.size(), 1u);
}

TEST(InBookSearchTest, MatchesConsecutiveOccurrencesInsideTrailingContext) {
  const std::string html = "<p>cat dog cat.</p>";
  std::vector<EpubSearchResult> matches;
  ASSERT_TRUE(EpubSearch::searchHtml(html, "cat", 0, "Ch 1", matches));
  ASSERT_EQ(matches.size(), 2u);
  EXPECT_EQ(matches[0].visibleTextOffset, 0u);
  EXPECT_EQ(matches[1].visibleTextOffset, 8u);
}

TEST(InBookSearchTest, SkipsHeadStyleAndDecodesEntitiesForVisibleOffset) {
  const std::string html =
      "<html><head><meta charset=\"utf-8\"><metadata><dc:title>Hidden margin</dc:title></metadata>"
      "<title>Hidden margin</title><style>p { margin: 0; }</style></head>\r\n"
      "<body><p>Wait&mdash;stop&#x21;<rp><span>(margin)</span></rp> Now margin here.</p></body></html>";

  std::vector<EpubSearchResult> matches;
  ASSERT_TRUE(EpubSearch::searchHtml(html, "margin", 2, "Chapter 3", matches));
  ASSERT_EQ(matches.size(), 1u);
  // "Wait" (4) + "—" (1) + "stop" (4) + "!" (1) + " Now " (5) = 15 codepoints before "margin"
  EXPECT_EQ(matches[0].visibleTextOffset, 15u);
  EXPECT_EQ(matches[0].preContext, "Wait—stop! Now ");
}

TEST(InBookSearchTest, MatchesOverlappingPrefixAndPreservesUtf8Boundaries) {
  const std::string html = "<p>Mississippi river</p>";
  std::vector<EpubSearchResult> matches;
  ASSERT_TRUE(EpubSearch::searchHtml(html, "issip", 0, "Ch 1", matches));
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches[0].visibleTextOffset, 4u);

  // Multi-byte UTF-8 characters (3-byte em-dashes) straddling the 24-byte leading/trailing context edges
  const std::string utf8Html = "<p>———————alpha———————</p>";
  std::vector<EpubSearchResult> utf8Matches;
  ASSERT_TRUE(EpubSearch::searchHtml(utf8Html, "alpha", 0, "Ch 1", utf8Matches));
  ASSERT_EQ(utf8Matches.size(), 1u);
  EXPECT_EQ(utf8Matches[0].visibleTextOffset, 7u);
  EXPECT_TRUE(isValidUtf8(utf8Matches[0].preContext));
  EXPECT_TRUE(isValidUtf8(utf8Matches[0].postContext));

  // Respects maxResults even when a (maxResults + 1)-th match occurs inside trailing context
  const std::string manyHtml = "<p>cat cat cat.</p>";
  std::vector<EpubSearchResult> cappedMatches;
  ASSERT_TRUE(EpubSearch::searchHtml(manyHtml, "cat", 0, "Ch 1", cappedMatches, 2));
  EXPECT_EQ(cappedMatches.size(), 2u);
}
