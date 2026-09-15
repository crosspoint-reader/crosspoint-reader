#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <string>
#include <vector>

namespace {

struct SearchMatch {
  uint32_t offset;
  std::string preContext;
  std::string match;
  std::string postContext;
};

std::vector<SearchMatch> simulateSearch(const std::string& htmlContent, const std::string& query) {
  std::vector<SearchMatch> matches;
  if (query.empty()) return matches;

  std::string lowerQuery = query;
  std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), [](unsigned char c) { return tolower(c); });

  bool insideTag = false;
  uint32_t visibleOffset = 0;
  size_t matchIndex = 0;
  std::deque<char> recentChars;
  bool collectingTrailing = false;
  uint32_t pendingOffset = 0;
  std::string pendingLeading;
  std::string trailingChars;

  char matchedSource[64] = {0};
  std::string pendingMatched;

  auto finalizeMatch = [&]() {
    collectingTrailing = false;
    matches.push_back({pendingOffset, pendingLeading, pendingMatched, trailingChars});
  };

  for (size_t i = 0; i < htmlContent.size(); ++i) {
    const char c = htmlContent[i];

    if (insideTag) {
      if (c == '>') insideTag = false;
      continue;
    }
    if (c == '<') {
      insideTag = true;
      continue;
    }

    // Visible character
    if ((static_cast<uint8_t>(c) & 0xC0) != 0x80) {
      visibleOffset++;
    }

    char norm = (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    recentChars.push_back(norm);
    if (recentChars.size() > 32) recentChars.pop_front();

    if (collectingTrailing) {
      trailingChars.push_back(norm);
      if (trailingChars.size() >= 16 || norm == '.') {
        finalizeMatch();
      }
    }

    char lowerC = static_cast<char>(tolower(static_cast<unsigned char>(norm)));
    if (lowerC == lowerQuery[matchIndex]) {
      if (matchIndex < sizeof(matchedSource)) {
        matchedSource[matchIndex] = norm;
      }
      matchIndex++;
      if (matchIndex == lowerQuery.length()) {
        if (collectingTrailing) {
          finalizeMatch();
        }
        pendingOffset = (visibleOffset >= lowerQuery.length()) ? (visibleOffset - lowerQuery.length()) : 0;
        pendingMatched = std::string(matchedSource, lowerQuery.length());
        pendingLeading = "";
        const size_t take = (recentChars.size() > lowerQuery.length()) ? (recentChars.size() - lowerQuery.length()) : 0;
        const size_t start = (take > 16) ? (take - 16) : 0;
        for (size_t k = start; k < take; ++k) pendingLeading += recentChars[k];

        trailingChars.clear();
        collectingTrailing = true;
        matchIndex = 0;
      }
    } else {
      if (matchIndex > 0) {
        matchIndex = (lowerC == lowerQuery[0]) ? 1 : 0;
        if (matchIndex == 1) {
          matchedSource[0] = norm;
        }
      }
    }
  }

  if (collectingTrailing) {
    finalizeMatch();
  }

  return matches;
}

}  // namespace

TEST(InBookSearchTest, StripsTagsAndMatchesText) {
  const std::string html = "<p>The quick <b>brown</b> fox jumps over the lazy dog.</p>";
  auto matches = simulateSearch(html, "brown");

  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches[0].match, "brown");
}

TEST(InBookSearchTest, CaseInsensitiveSearch) {
  const std::string html = "<h1>CHAPTER ONE</h1><p>Captain Nemo looked out the window.</p>";

  // Search with lowercase, verifies source casing "Nemo" is preserved in match
  auto matches = simulateSearch(html, "nemo");
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches[0].match, "Nemo");

  // Search with uppercase, verifies source casing "Captain" is preserved
  auto matches2 = simulateSearch(html, "CAPTAIN");
  ASSERT_EQ(matches2.size(), 1u);
  EXPECT_EQ(matches2[0].match, "Captain");
}

TEST(InBookSearchTest, DoesNotMatchInsideHtmlTags) {
  const std::string html = "<div class=\"important-class\" id=\"chapter_box\">Normal text here.</div>";

  // "class" and "important" are inside tags, should not match
  auto matches = simulateSearch(html, "important");
  EXPECT_EQ(matches.size(), 0u);

  // "Normal" is visible text, should match
  auto matches2 = simulateSearch(html, "Normal");
  EXPECT_EQ(matches2.size(), 1u);
}

TEST(InBookSearchTest, MatchesConsecutiveOccurrencesInsideTrailingContext) {
  const std::string html = "<p>cat dog cat.</p>";
  auto matches = simulateSearch(html, "cat");
  ASSERT_EQ(matches.size(), 2u);
  EXPECT_EQ(matches[0].offset, 0u);
  EXPECT_EQ(matches[1].offset, 8u);
}
