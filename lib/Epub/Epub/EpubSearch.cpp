#include "EpubSearch.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <Epub.h>
#include <I18n.h>
#include <Logging.h>
#include <Print.h>
#include <esp_timer.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "VisibleTextUtils.h"
#include "htmlEntities.h"

namespace {

size_t countUtf8Codepoints(const std::string_view text) {
  size_t count = 0;
  for (const char ch : text) {
    if ((static_cast<uint8_t>(ch) & 0xC0) != 0x80) {
      ++count;
    }
  }
  return count;
}

size_t skipLeadingContinuationBytes(const char* buf, size_t start, const size_t end) {
  while (start < end && (static_cast<uint8_t>(buf[start]) & 0xC0) == 0x80) {
    ++start;
  }
  return start;
}

size_t trimTrailingIncompleteUtf8(const char* buf, const size_t len) {
  if (len == 0) return 0;
  const size_t maxBack = std::min<size_t>(len, 4);
  for (size_t k = 1; k <= maxBack; ++k) {
    const uint8_t b = static_cast<uint8_t>(buf[len - k]);
    if ((b & 0xC0) == 0x80) {
      continue;
    }
    if ((b & 0x80) == 0) {
      return len;
    }
    size_t expected = 1;
    if ((b & 0xE0) == 0xC0) {
      expected = 2;
    } else if ((b & 0xF0) == 0xE0) {
      expected = 3;
    } else if ((b & 0xF8) == 0xF0) {
      expected = 4;
    }
    return (k < expected) ? (len - k) : len;
  }
  return 0;
}

size_t encodeUtf8Codepoint(const uint32_t cp, char out[4]) {
  if (cp <= 0x7F) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp <= 0x7FF) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp <= 0xFFFF) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  if (cp <= 0x10FFFF) {
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
  }
  return 0;
}

class EpubSearchStreamer final
#ifdef ARDUINO
    : public Print
#endif
{
 public:
  EpubSearchStreamer(const std::string_view lowerQuery, const uint32_t queryCodepoints, const int spineIndex,
                     const std::string_view chapterTitle, std::vector<EpubSearchResult>& results,
                     const size_t maxResults)
      : lowerQuery(lowerQuery),
        queryCodepoints(queryCodepoints),
        spineIndex(spineIndex),
        chapterTitle(chapterTitle),
        results(results),
        maxResults(maxResults) {}

  size_t write(const uint8_t b)
#ifdef ARDUINO
      override
#endif
  {
    if (finished || results.size() >= maxResults) {
      finished = true;
      return 0;  // Signal early stop to zip streamer
    }

    const char c = static_cast<char>(b);

    if (inEntity) {
      if (entityLen + 1 < sizeof(entityBuffer)) {
        entityBuffer[entityLen++] = c;
      } else {
        flushEntityAsLiteral();
        inEntity = false;
        entityLen = 0;
      }
      if (inEntity) {
        if (c == ';') {
          finishEntity();
        } else if (c == '<' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
          const bool startsTag = (c == '<');
          if (startsTag) {
            --entityLen;
          }
          flushEntityAsLiteral();
          inEntity = false;
          entityLen = 0;
          if (startsTag) {
            beginTag();
          }
        }
      }
      return 1;
    }

    if (tagState != TAG_NONE) {
      processByteInTag(b);
      return 1;
    }

    if (c == '<') {
      prevCR = false;
      beginTag();
      return 1;
    }

    if ((sawDocumentRoot && !insideBody) || nonVisibleDepth > 0) {
      prevCR = false;
      return 1;
    }

    if (c == '&') {
      prevCR = false;
      inEntity = true;
      entityBuffer[0] = '&';
      entityLen = 1;
      return 1;
    }

    // XML 1.0 line-ending normalization: collapse CRLF to a single LF so visibleTextOffset matches Expat's page LUT.
    const bool afterCR = prevCR;
    prevCR = (c == '\r');
    if (c == '\n' && afterCR) {
      return 1;
    }

    processVisibleByte(b);
    return 1;
  }

  size_t write(const uint8_t* buffer, const size_t size)
#ifdef ARDUINO
      override
#endif
  {
    if (!buffer || size == 0 || finished) return 0;
    for (size_t i = 0; i < size; ++i) {
      if (write(buffer[i]) == 0) {
        return i;
      }
#ifdef ARDUINO
      if (++bytesSinceYield >= YIELD_BYTE_INTERVAL) {
        bytesSinceYield = 0;
        delay(1);
      }
#endif
    }
    return size;
  }

  void finishPending() {
    if (inEntity) {
      flushEntityAsLiteral();
      inEntity = false;
      entityLen = 0;
    }
    if (collectingTrailing) {
      finalizePendingMatch();
    }
  }

  bool isFinished() const { return finished || results.size() >= maxResults; }

 private:
  enum TagState : uint8_t { TAG_NONE, TAG_START, TAG_NAME, TAG_ATTRS, TAG_COMMENT_OR_DECL };

  void beginTag() {
    tagState = TAG_START;
    tagNameLen = 0;
    tagIsClose = false;
    tagSelfClose = false;
    inAttrQuote = false;
    attrQuoteChar = 0;
  }

  void processByteInTag(const uint8_t b) {
    const char c = static_cast<char>(b);
    switch (tagState) {
      case TAG_START:
        if (c == '/') {
          tagIsClose = true;
          tagState = TAG_NAME;
        } else if (c == '!' || c == '?') {
          tagState = TAG_COMMENT_OR_DECL;
        } else if (c == '>') {
          tagState = TAG_NONE;
        } else {
          tagName[0] = c;
          tagNameLen = 1;
          tagState = TAG_NAME;
        }
        break;
      case TAG_NAME:
        if (c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
          if (c == '/' && !tagIsClose) {
            tagSelfClose = true;
          }
          if (c == '>') {
            finishTag();
            tagState = TAG_NONE;
          } else {
            tagState = TAG_ATTRS;
          }
        } else if (tagNameLen + 1 < sizeof(tagName)) {
          tagName[tagNameLen++] = c;
        }
        break;
      case TAG_ATTRS:
        if (!inAttrQuote) {
          if (c == '"' || c == '\'') {
            inAttrQuote = true;
            attrQuoteChar = c;
          } else if (c == '/' && !tagIsClose) {
            tagSelfClose = true;
          } else if (c == '>') {
            finishTag();
            tagState = TAG_NONE;
          }
        } else if (c == attrQuoteChar) {
          inAttrQuote = false;
          attrQuoteChar = 0;
        }
        break;
      case TAG_COMMENT_OR_DECL:
        if (c == '>') {
          tagState = TAG_NONE;
        }
        break;
      case TAG_NONE:
        break;
    }
  }

  void finishTag() {
    const std::string_view name(tagName, tagNameLen);
    if (!tagIsClose) {
      if (VisibleTextUtils::equalsTag(name, "html") || VisibleTextUtils::equalsTag(name, "head")) {
        sawDocumentRoot = true;
      } else if (VisibleTextUtils::equalsTag(name, "body")) {
        sawDocumentRoot = true;
        insideBody = true;
        nonVisibleDepth = 0;
      }
      if (!tagSelfClose && VisibleTextUtils::isNonVisibleElement(name)) {
        ++nonVisibleDepth;
      }
    } else {
      if (VisibleTextUtils::equalsTag(name, "body")) {
        insideBody = false;
      } else if (nonVisibleDepth > 0 && VisibleTextUtils::isNonVisibleElement(name)) {
        --nonVisibleDepth;
      }
    }
  }

  void flushEntityAsLiteral() {
    for (size_t i = 0; i < entityLen && !finished; ++i) {
      processVisibleByte(static_cast<uint8_t>(entityBuffer[i]));
    }
  }

  void finishEntity() {
    entityBuffer[entityLen] = '\0';
    inEntity = false;
    const size_t len = entityLen;
    entityLen = 0;

    if (const char* resolved = lookupHtmlEntity(entityBuffer, len)) {
      for (const char* p = resolved; *p != '\0' && !finished; ++p) {
        processVisibleByte(static_cast<uint8_t>(*p));
      }
      return;
    }

    if (len >= 4 && entityBuffer[1] == '#' && entityBuffer[len - 1] == ';') {
      char* endPtr = nullptr;
      unsigned long cp = 0;
      if (entityBuffer[2] == 'x' || entityBuffer[2] == 'X') {
        cp = std::strtoul(entityBuffer + 3, &endPtr, 16);
      } else {
        cp = std::strtoul(entityBuffer + 2, &endPtr, 10);
      }
      if (endPtr == entityBuffer + len - 1 && cp > 0 && cp <= 0x10FFFF) {
        char utf8[4];
        const size_t utf8Len = encodeUtf8Codepoint(static_cast<uint32_t>(cp), utf8);
        for (size_t i = 0; i < utf8Len && !finished; ++i) {
          processVisibleByte(static_cast<uint8_t>(utf8[i]));
        }
        return;
      }
    }

    for (size_t i = 0; i < len && !finished; ++i) {
      processVisibleByte(static_cast<uint8_t>(entityBuffer[i]));
    }
  }

  void processVisibleByte(const uint8_t b) {
    if (finished) return;

    if ((b & 0xC0) != 0x80) {
      ++visibleTextOffset;
    }

    const char c = static_cast<char>(b);
    char normalizedChar = c;
    if (c == '\r' || c == '\n' || c == '\t') {
      normalizedChar = ' ';
    }

    if (recentCharsLen < sizeof(recentChars)) {
      recentChars[recentCharsLen++] = normalizedChar;
    } else {
      std::memmove(recentChars, recentChars + 1, sizeof(recentChars) - 1);
      recentChars[sizeof(recentChars) - 1] = normalizedChar;
    }

    if (collectingTrailing) {
      if (trailingCharsLen < sizeof(trailingChars)) {
        trailingChars[trailingCharsLen++] = normalizedChar;
      }
      if (trailingCharsLen >= TRAILING_CONTEXT_CHARS || normalizedChar == '.') {
        finalizePendingMatch();
        if (finished) return;
      }
    }

    const char lowercasedChar = static_cast<char>(std::tolower(static_cast<unsigned char>(normalizedChar)));
    if (lowercasedChar == lowerQuery[matchIndex]) {
      if (matchIndex < sizeof(matchedSource)) {
        matchedSource[matchIndex] = normalizedChar;
      }
      ++matchIndex;
      if (matchIndex == lowerQuery.length()) {
        if (collectingTrailing) {
          finalizePendingMatch();
          if (finished) return;
        }

        pendingMatchOffset = (visibleTextOffset >= queryCodepoints) ? (visibleTextOffset - queryCodepoints) : 0;
        pendingLeadingLen = 0;
        const size_t contextToTake =
            (recentCharsLen > lowerQuery.length()) ? (recentCharsLen - lowerQuery.length()) : 0;
        size_t startIdx = (contextToTake > LEADING_CONTEXT_CHARS) ? (contextToTake - LEADING_CONTEXT_CHARS) : 0;
        startIdx = skipLeadingContinuationBytes(recentChars, startIdx, contextToTake);
        for (size_t i = startIdx; i < contextToTake && pendingLeadingLen < sizeof(pendingLeading); ++i) {
          pendingLeading[pendingLeadingLen++] = recentChars[i];
        }

        pendingMatchedLen = std::min(matchIndex, sizeof(pendingMatched));
        std::memcpy(pendingMatched, matchedSource, pendingMatchedLen);

        trailingCharsLen = 0;
        collectingTrailing = true;
        matchIndex = 0;
      }
    } else if (matchIndex > 0) {
      if (matchIndex < sizeof(matchedSource)) {
        matchedSource[matchIndex] = normalizedChar;
      }
      const size_t candidateTotal = std::min(matchIndex + 1, sizeof(matchedSource));
      size_t newLen = 0;
      for (size_t len = std::min(matchIndex, lowerQuery.length()); len >= 1; --len) {
        const size_t offset = candidateTotal - len;
        bool prefixMatch = true;
        for (size_t k = 0; k < len; ++k) {
          const char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(matchedSource[offset + k])));
          if (lc != lowerQuery[k]) {
            prefixMatch = false;
            break;
          }
        }
        if (prefixMatch) {
          newLen = len;
          std::memmove(matchedSource, matchedSource + offset, len);
          break;
        }
      }
      matchIndex = newLen;
    }
  }

  void finalizePendingMatch() {
    collectingTrailing = false;

    const size_t safeTrailingLen = trimTrailingIncompleteUtf8(trailingChars, trailingCharsLen);
    EpubSearchResult result;
    result.spineIndex = spineIndex;
    result.visibleTextOffset = pendingMatchOffset;
    result.chapterTitle.assign(chapterTitle);
    result.preContext.assign(pendingLeading, pendingLeadingLen);
    result.match.assign(pendingMatched, pendingMatchedLen);
    result.postContext.assign(trailingChars, safeTrailingLen);

    results.push_back(std::move(result));

    if (results.size() >= maxResults) {
      finished = true;
    }
  }

  static constexpr size_t MAX_CONTEXT_CHARS = 88;
  static constexpr size_t LEADING_CONTEXT_CHARS = 24;
  static constexpr size_t TRAILING_CONTEXT_CHARS = 24;
  static constexpr size_t MAX_MATCH_CHARS = 64;
#ifdef ARDUINO
  static constexpr size_t YIELD_BYTE_INTERVAL = 16384;
#endif

  std::string_view lowerQuery;
  uint32_t queryCodepoints = 0;
  int spineIndex = 0;
  std::string_view chapterTitle;
  std::vector<EpubSearchResult>& results;
  size_t maxResults = 30;

  TagState tagState = TAG_NONE;
  bool tagIsClose = false;
  bool tagSelfClose = false;
  bool inAttrQuote = false;
  char attrQuoteChar = 0;
  char tagName[16] = {0};
  size_t tagNameLen = 0;
  bool sawDocumentRoot = false;
  bool insideBody = false;
  uint8_t nonVisibleDepth = 0;

  bool inEntity = false;
  char entityBuffer[16] = {0};
  size_t entityLen = 0;
  bool prevCR = false;

  uint32_t visibleTextOffset = 0;
  size_t matchIndex = 0;
  bool finished = false;
#ifdef ARDUINO
  size_t bytesSinceYield = 0;
#endif

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

bool EpubSearch::searchHtml(const std::string_view html, const std::string_view query, const int spineIndex,
                            const std::string_view chapterTitle, std::vector<EpubSearchResult>& results,
                            const size_t maxResults) {
  if (query.empty() || maxResults == 0) return false;

  std::string lowerQuery(query.substr(0, std::min<size_t>(query.size(), 64)));
  std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const uint32_t queryCodepoints = static_cast<uint32_t>(countUtf8Codepoints(lowerQuery));

  const size_t before = results.size();
  EpubSearchStreamer streamer(lowerQuery, queryCodepoints, spineIndex, chapterTitle, results, maxResults);
  streamer.write(reinterpret_cast<const uint8_t*>(html.data()), html.size());
  streamer.finishPending();
  return results.size() > before;
}

#ifdef ARDUINO
bool EpubSearch::search(const Epub& epub, const std::string& query, std::vector<EpubSearchResult>& results,
                        const size_t maxResults) {
  if (query.empty() || maxResults == 0) return false;

  std::string lowerQuery(query.substr(0, std::min<size_t>(query.size(), 64)));
  std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const uint32_t queryCodepoints = static_cast<uint32_t>(countUtf8Codepoints(lowerQuery));

  const int spineCount = epub.getSpineItemsCount();
  results.clear();
  results.reserve(std::min(maxResults, static_cast<size_t>(32)));

  const int64_t searchStartUs = esp_timer_get_time();

  for (int i = 0; i < spineCount; ++i) {
    if (results.size() >= maxResults) {
      break;
    }

    const std::string href = epub.getSpineItem(i).href;
    if (href.empty()) continue;

    const size_t countBefore = results.size();
    EpubSearchStreamer streamer(lowerQuery, queryCodepoints, i, "", results, maxResults);
    epub.readItemContentsToStream(href, streamer, 2048, /*allowEarlyStop=*/true);
    streamer.finishPending();

    if (results.size() > countBefore) {
      const int tocIdx = epub.getTocIndexForSpineIndex(i);
      std::string title = (tocIdx != -1) ? epub.getTocItem(tocIdx).title : "";
      if (title.empty()) {
        char sectionBuf[32];
        snprintf(sectionBuf, sizeof(sectionBuf), "%s%d", tr(STR_SECTION_PREFIX), i + 1);
        title = sectionBuf;
      }
      for (size_t j = countBefore; j < results.size(); ++j) {
        results[j].chapterTitle = title;
      }
    }

    // Yield tick to avoid starving watchdogs on long books
    delay(1);
  }

  const int64_t elapsedMs = (esp_timer_get_time() - searchStartUs) / 1000;
  LOG_INF("SRCH", "Search for \"%s\" across %d spine items: %lld ms, %zu result(s)", query.c_str(), spineCount,
          elapsedMs, results.size());

  return !results.empty();
}
#endif
