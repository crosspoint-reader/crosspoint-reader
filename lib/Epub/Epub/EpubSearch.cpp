#include "EpubSearch.h"

#include <Arduino.h>
#include <Epub.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_timer.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

class EpubSearchStreamer final : public Print {
 public:
  EpubSearchStreamer(const std::string& lowerQuery, int spineIndex, const std::string& chapterTitle,
                     std::vector<EpubSearchResult>& results, size_t maxResults)
      : lowerQuery(lowerQuery),
        spineIndex(spineIndex),
        chapterTitle(chapterTitle),
        results(results),
        maxResults(maxResults) {}

  size_t write(uint8_t b) override {
    if (finished || results.size() >= maxResults) {
      finished = true;
      return 0;  // Signal early stop to zip streamer
    }

    const char c = static_cast<char>(b);

    if (insideTag) {
      if (c == '>') {
        insideTag = false;
      }
      return 1;
    }

    if (c == '<') {
      insideTag = true;
      return 1;
    }

    // Process visible body character
    // Count UTF-8 codepoints (non-continuation bytes)
    if ((b & 0xC0) != 0x80) {
      visibleTextOffset++;
    }

    // Normalize whitespace
    char normalizedChar = c;
    if (c == '\r' || c == '\n' || c == '\t') {
      normalizedChar = ' ';
    }

    // Keep rolling history of visible characters for snippet context
    if (recentCharsLen < sizeof(recentChars)) {
      recentChars[recentCharsLen++] = normalizedChar;
    } else {
      memmove(recentChars, recentChars + 1, sizeof(recentChars) - 1);
      recentChars[sizeof(recentChars) - 1] = normalizedChar;
    }

    // If currently collecting trailing context after a match
    if (collectingTrailing) {
      if (trailingCharsLen < sizeof(trailingChars)) {
        trailingChars[trailingCharsLen++] = normalizedChar;
      }
      if (trailingCharsLen >= TRAILING_CONTEXT_CHARS || normalizedChar == '.') {
        finalizePendingMatch();
      }
      // Note: do not return here so matching continues for subsequent matches
    }

    // Check match against lowercase query
    const char lowerC = static_cast<char>(tolower(static_cast<unsigned char>(normalizedChar)));
    if (lowerC == lowerQuery[matchIndex]) {
      if (matchIndex < sizeof(matchedSource)) {
        matchedSource[matchIndex] = normalizedChar;
      }
      matchIndex++;
      if (matchIndex == lowerQuery.length()) {
        // If an earlier match was still collecting trailing context, finalize it now
        if (collectingTrailing) {
          finalizePendingMatch();
        }

        // Complete match found!
        pendingMatchOffset = (visibleTextOffset >= lowerQuery.length()) ? (visibleTextOffset - lowerQuery.length()) : 0;

        // Capture matched source text preserving source casing
        pendingMatchedLen = std::min(lowerQuery.length(), sizeof(pendingMatched));
        memcpy(pendingMatched, matchedSource, pendingMatchedLen);

        // Extract leading context from recentChars
        pendingLeadingLen = 0;
        const size_t contextToTake =
            (recentCharsLen > lowerQuery.length()) ? (recentCharsLen - lowerQuery.length()) : 0;
        const size_t startIdx = (contextToTake > LEADING_CONTEXT_CHARS) ? (contextToTake - LEADING_CONTEXT_CHARS) : 0;
        for (size_t i = startIdx; i < contextToTake && pendingLeadingLen < sizeof(pendingLeading); ++i) {
          pendingLeading[pendingLeadingLen++] = recentChars[i];
        }

        // Start collecting trailing context
        trailingCharsLen = 0;
        collectingTrailing = true;
        matchIndex = 0;
      }
    } else {
      // Mismatch, reset matchIndex
      if (matchIndex > 0) {
        matchIndex = (lowerC == lowerQuery[0]) ? 1 : 0;
        if (matchIndex == 1) {
          matchedSource[0] = normalizedChar;
        }
      }
    }

    return 1;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!buffer || size == 0 || finished) return 0;
    for (size_t i = 0; i < size; ++i) {
      if (write(buffer[i]) == 0) {
        return i;
      }
    }
    return size;
  }

  void finishPending() {
    if (collectingTrailing) {
      finalizePendingMatch();
    }
  }

  bool isFinished() const { return finished || results.size() >= maxResults; }

 private:
  void finalizePendingMatch() {
    collectingTrailing = false;

    EpubSearchResult result;
    result.spineIndex = spineIndex;
    result.visibleTextOffset = pendingMatchOffset;
    result.chapterTitle = chapterTitle;
    result.preContext.assign(pendingLeading, pendingLeadingLen);
    result.match.assign(pendingMatched, pendingMatchedLen);
    result.postContext.assign(trailingChars, trailingCharsLen);

    results.push_back(std::move(result));

    if (results.size() >= maxResults) {
      finished = true;
    }
  }

  static constexpr size_t MAX_CONTEXT_CHARS = 48;
  static constexpr size_t LEADING_CONTEXT_CHARS = 24;
  static constexpr size_t TRAILING_CONTEXT_CHARS = 24;
  static constexpr size_t MAX_MATCH_CHARS = 64;

  const std::string lowerQuery;
  int spineIndex;
  std::string chapterTitle;
  std::vector<EpubSearchResult>& results;
  size_t maxResults;

  bool insideTag = false;
  uint32_t visibleTextOffset = 0;
  size_t matchIndex = 0;
  bool finished = false;

  char recentChars[MAX_CONTEXT_CHARS] = {0};
  size_t recentCharsLen = 0;
  bool collectingTrailing = false;
  uint32_t pendingMatchOffset = 0;
  char pendingLeading[LEADING_CONTEXT_CHARS] = {0};
  size_t pendingLeadingLen = 0;
  char matchedSource[MAX_MATCH_CHARS] = {0};
  char pendingMatched[MAX_MATCH_CHARS] = {0};
  size_t pendingMatchedLen = 0;
  char trailingChars[TRAILING_CONTEXT_CHARS] = {0};
  size_t trailingCharsLen = 0;
};

}  // namespace

bool EpubSearch::search(const Epub& epub, const std::string& query, std::vector<EpubSearchResult>& results,
                        const size_t maxResults) {
  if (query.empty()) return false;

  std::string lowerQuery = query;
  std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), [](unsigned char c) { return tolower(c); });

  const int spineCount = epub.getSpineItemsCount();
  results.clear();
  results.reserve(std::min(maxResults, static_cast<size_t>(32)));

  const int64_t searchStartUs = esp_timer_get_time();

  for (int i = 0; i < spineCount; ++i) {
    if (results.size() >= maxResults) {
      break;
    }

    const int tocIdx = epub.getTocIndexForSpineIndex(i);
    std::string title = (tocIdx != -1) ? epub.getTocItem(tocIdx).title : "";
    if (title.empty()) {
      char sectionBuf[32];
      snprintf(sectionBuf, sizeof(sectionBuf), "%s%d", tr(STR_SECTION_PREFIX), i + 1);
      title = sectionBuf;
    }

    const std::string href = epub.getSpineItem(i).href;
    if (href.empty()) continue;

    EpubSearchStreamer streamer(lowerQuery, i, title, results, maxResults);
    epub.readItemContentsToStream(href, streamer, 2048, /*allowEarlyStop=*/true);
    streamer.finishPending();

    // Yield tick to avoid starving watchdogs on long books
    delay(1);
  }

  const int64_t elapsedMs = (esp_timer_get_time() - searchStartUs) / 1000;
  LOG_INF("SRCH", "Search for \"%s\" across %d spine items: %lld ms, %zu result(s)", query.c_str(), spineCount,
          elapsedMs, results.size());

  return !results.empty();
}
