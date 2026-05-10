#include "ChapterXPathReverseMapper.h"

#include <HalStorage.h>
#include <Logging.h>
#include <expat.h>

#include <algorithm>
#include <cstdlib>
#include <string>

#include "ChapterXPathIndexerInternal.h"
#include "ChapterXPathIndexerState.h"

namespace ChapterXPathIndexerInternal {

namespace {

// Reverse mapper: translate KOReader XPath to intra-spine progress.
// Matching preference order is strict and deterministic:
//   exact > exact-no-index > ancestor > ancestor-no-index.
// For /text()[N].M, M is treated as codepoint offset and converted back to
// internal visible-byte progress.

enum class MatchTier : int {
  NONE = 0,
  ANCESTOR_NO_IDX = 1,
  ANCESTOR = 2,
  EXACT_NO_IDX = 3,
  EXACT = 4,
};

struct ReverseState : StackState {
  int spineIndex;
  std::string targetNorm;
  std::string targetNoIndex;

  int targetTextNodeIndex = 0;
  int targetCharOffset = 0;
  bool inParentTextNode = false;
  size_t codepointsInCurrentTextNode = 0;
  int currentTextNodeCount = 0;

  // Running <li> count at any depth. Mirrors xpathListItemIndex in ChapterHtmlSlimParser
  // so the index captured at match time can be used as a key into the section's li LUT.
  int liCount = 0;
  // True when the deepest element of targetNorm is /li[N]. Only then is bestLiIndex
  // meaningful — for <p>-anchored targets we use the existing paragraph index path.
  bool targetEndsInLi = false;

  MatchTier bestTier = MatchTier::NONE;
  int bestDepth = -1;
  size_t bestOffset = 0;
  bool bestExact = false;
  const char* bestTierName = nullptr;
  // Snapshot of liCount at the moment the best match was captured. The reverse
  // mapper surfaces this so the runtime can call Section::getPageForListItemIndex()
  // and snap a list-item XPath to the precise page on download.
  int bestLiIndex = 0;

  ReverseState(const int spineIndex, const std::string& xpath) : spineIndex(spineIndex) {
    // Parse optional text-node suffix before normalizing for element matching.
    // KOReader emits two shapes that both land here:
    //   /text()[N].M  — explicit 1-based text-node index + codepoint offset
    //   /text().M     — implicit first text-node (N=1) + codepoint offset
    std::string raw = xpath;
    for (char& c : raw) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string tnPat = "/text()";
    const size_t tnPos = raw.rfind(tnPat);
    if (tnPos != std::string::npos) {
      size_t cursor = tnPos + tnPat.size();
      int nodeIdx = 1;
      bool valid = true;
      if (cursor < raw.size() && raw[cursor] == '[') {
        cursor++;
        size_t numEnd = cursor;
        while (numEnd < raw.size() && std::isdigit(static_cast<unsigned char>(raw[numEnd]))) {
          numEnd++;
        }
        if (numEnd > cursor && numEnd < raw.size() && raw[numEnd] == ']') {
          nodeIdx = static_cast<int>(std::strtol(raw.substr(cursor, numEnd - cursor).c_str(), nullptr, 10));
          cursor = numEnd + 1;
        } else {
          valid = false;
        }
      }
      if (valid && nodeIdx >= 1) {
        targetTextNodeIndex = nodeIdx;
        if (cursor < raw.size() && raw[cursor] == '.') {
          cursor++;
          size_t charEnd = cursor;
          while (charEnd < raw.size() && std::isdigit(static_cast<unsigned char>(raw[charEnd]))) {
            charEnd++;
          }
          if (charEnd > cursor) {
            const long charOff = std::strtol(raw.substr(cursor, charEnd - cursor).c_str(), nullptr, 10);
            if (charOff >= 0) {
              targetCharOffset = static_cast<int>(charOff);
            }
          }
        }
      }
    }
    targetNorm = normalizeXPath(xpath);
    targetNoIndex = removeIndices(targetNorm);

    // Detect /li[N] as the deepest segment of targetNorm. normalizeXPath has already
    // stripped any /text() and /text()[N].M suffix and lower-cased the tag names, so
    // a simple tail check is enough.
    const size_t lastSlash = targetNorm.rfind('/');
    if (lastSlash != std::string::npos) {
      const std::string tail = targetNorm.substr(lastSlash + 1);
      if (tail.size() >= 2 && tail.compare(0, 2, "li") == 0 && (tail.size() == 2 || tail[2] == '[')) {
        targetEndsInLi = true;
      }
    }
  }

  void onStartElement(const XML_Char* rawName) {
    inParentTextNode = false;
    pushElement(rawName);
    // Increment after pushElement so stack.back().tag is already lowercased and
    // matches the parser-side counter, which also fires on startElement.
    if (!stack.empty() && stack.back().tag == "li") {
      liCount++;
    }
  }

  void onEndElement() {
    // Empty/textless elements can still be a valid anchor location.
    if (!stack.empty() && !stack.back().hasText) {
      checkMatch();
    }
    inParentTextNode = false;
    popElement();
  }

  void onCharData(const XML_Char* text, const int len) {
    if (shouldSkipText(len)) {
      return;
    }

    const size_t visible = countVisibleBytes(text, len);
    const size_t codepoints = countUtf8Codepoints(text, len);

    if (targetTextNodeIndex > 0 && !stack.empty()) {
      const std::string xpath = normalizeXPath(currentXPath(spineIndex));
      if (xpath == targetNorm) {
        stack.back().hasText = true;
        if (!inParentTextNode) {
          inParentTextNode = true;
          currentTextNodeCount++;
          codepointsInCurrentTextNode = 0;
        }
        if (currentTextNodeCount == targetTextNodeIndex && bestTier < MatchTier::EXACT) {
          const size_t charOff = static_cast<size_t>(targetCharOffset);
          if (charOff >= codepointsInCurrentTextNode && charOff <= codepointsInCurrentTextNode + codepoints) {
            const size_t cpInChunk = charOff - codepointsInCurrentTextNode;
            const size_t pos = totalTextBytes + visibleBytesBeforeCodepoint(text, len, cpInChunk);
            bestTier = MatchTier::EXACT;
            bestDepth = pathDepth(xpath);
            bestOffset = pos;
            bestExact = true;
            bestTierName = "text-node-exact";
            bestLiIndex = liCount;
          }
        }
        codepointsInCurrentTextNode += codepoints;
        totalTextBytes += visible;
        return;
      }
    }

    if (isWhitespaceOnly(text, len)) {
      return;
    }

    if (!stack.empty() && !stack.back().hasText) {
      stack.back().hasText = true;
      checkMatch();
    }

    totalTextBytes += visible;
  }

  void checkMatch() {
    const std::string xpath = normalizeXPath(currentXPath(spineIndex));
    const int depth = pathDepth(xpath);

    const bool targetIsTextSelector = targetTextNodeIndex > 0;

    if (xpath == targetNorm) {
      // For /text()[N].M targets, the normalized parent element path is equal to
      // targetNorm. Treat that as an ancestor-level anchor so text-node exact
      // matching can still determine the real intra-node offset.
      if (targetIsTextSelector) {
        tryUpdate(MatchTier::ANCESTOR, depth, "text-parent", false);
      } else {
        tryUpdate(MatchTier::EXACT, depth, "exact", true);
      }
      return;
    }
    if (isAncestorPath(xpath, targetNorm)) {
      tryUpdate(MatchTier::ANCESTOR, depth, "ancestor", false);
      return;
    }

    const std::string xpathNoIdx = removeIndices(xpath);
    if (xpathNoIdx == targetNoIndex) {
      tryUpdate(MatchTier::EXACT_NO_IDX, depth, "index-insensitive", false);
    } else if (isAncestorPath(xpathNoIdx, targetNoIndex)) {
      tryUpdate(MatchTier::ANCESTOR_NO_IDX, depth, "index-insensitive-ancestor", false);
    }
  }

  void tryUpdate(const MatchTier tier, const int depth, const char* tierName, const bool isExact) {
    if (tier > bestTier || (tier == bestTier && depth > bestDepth)) {
      bestTier = tier;
      bestDepth = depth;
      bestOffset = totalTextBytes;
      bestExact = isExact;
      bestTierName = tierName;
      bestLiIndex = liCount;
    }
  }
};

}  // namespace

bool findProgressForXPathInternal(const std::shared_ptr<Epub>& epub, const int spineIndex, const std::string& xpath,
                                  float& outIntraSpineProgress, bool& outExactMatch, uint16_t* outListItemIndex) {
  outIntraSpineProgress = 0.0f;
  outExactMatch = false;
  if (outListItemIndex) {
    *outListItemIndex = 0;
  }

  if (xpath.empty()) {
    return false;
  }

  const std::string tmpPath = decompressToTempFile(epub, spineIndex);
  if (tmpPath.empty()) {
    return false;
  }

  ReverseState state(spineIndex, xpath);
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    Storage.remove(tmpPath.c_str());
    return false;
  }

  XML_SetUserData(parser, &state);
  XML_SetElementHandler(parser, parserStartCb<ReverseState>, parserEndCb<ReverseState>);
  XML_SetCharacterDataHandler(parser, parserCharCb<ReverseState>);
  XML_SetDefaultHandlerExpand(parser, parserDefaultCb<ReverseState>);
  const bool parseOk = runParse(parser, tmpPath);

  if (!parseOk) {
    LOG_ERR("KOX", "XPath parse failed for spine=%d at line %lu: %s", spineIndex, XML_GetCurrentLineNumber(parser),
            XML_ErrorString(XML_GetErrorCode(parser)));
  }
  XML_ParserFree(parser);
  Storage.remove(tmpPath.c_str());

  if (!parseOk || state.bestTier == MatchTier::NONE) {
    LOG_DBG("KOX", "Reverse: spine=%d no match for '%s'", spineIndex, xpath.c_str());
    return false;
  }

  outExactMatch = state.bestExact;
  if (state.totalTextBytes == 0) {
    outIntraSpineProgress = 0.0f;
  } else {
    outIntraSpineProgress = static_cast<float>(state.bestOffset) / static_cast<float>(state.totalTextBytes);
    outIntraSpineProgress = std::max(0.0f, std::min(1.0f, outIntraSpineProgress));
  }

  // Only surface the li index when the target was actually <li>-anchored AND we
  // captured a count > 0. NO_IDX fallback tiers can match the wrong <li> sibling,
  // so restrict to tiers that imply we were inside the target element itself.
  if (outListItemIndex && state.targetEndsInLi && state.bestLiIndex > 0 &&
      (state.bestTier == MatchTier::EXACT || state.bestTier == MatchTier::ANCESTOR) &&
      state.bestLiIndex <= UINT16_MAX) {
    *outListItemIndex = static_cast<uint16_t>(state.bestLiIndex);
  }

  if (state.targetTextNodeIndex > 0) {
    LOG_DBG("KOX", "Reverse: spine=%d %s match textNode=%d char=%d offset=%zu/%zu -> progress=%.3f li=%d for '%s'",
            spineIndex, state.bestTierName, state.targetTextNodeIndex, state.targetCharOffset, state.bestOffset,
            state.totalTextBytes, outIntraSpineProgress, state.bestLiIndex, xpath.c_str());
  } else {
    LOG_DBG("KOX", "Reverse: spine=%d %s match offset=%zu/%zu -> progress=%.3f li=%d for '%s'", spineIndex,
            state.bestTierName, state.bestOffset, state.totalTextBytes, outIntraSpineProgress, state.bestLiIndex,
            xpath.c_str());
  }
  return true;
}

}  // namespace ChapterXPathIndexerInternal
