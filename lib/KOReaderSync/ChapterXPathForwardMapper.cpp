#include "ChapterXPathForwardMapper.h"

#include <HalStorage.h>
#include <Logging.h>
#include <expat.h>

#include <algorithm>
#include <string>
#include <unordered_map>

#include "ChapterXPathIndexerInternal.h"
#include "ChapterXPathIndexerState.h"

namespace ChapterXPathIndexerInternal {

namespace {

// Forward mapper: translate intra-spine progress to a KOReader-compatible XPath.
// Strategy:
// 1) Count total visible text bytes in chapter.
// 2) Stream parse again and stop when target byte offset is reached.
// 3) Emit either an element path or /text()[N].M when at body text-node level.

struct ForwardState : StackState {
  int spineIndex;
  size_t targetOffset;
  std::string result;
  bool found = false;
  XML_Parser parser = nullptr;

  int bodyTextNodeCount = 0;
  size_t codepointsInBodyTextNode = 0;
  bool inBodyTextNode = false;

  ForwardState(const int spineIndex, const size_t targetOffset) : spineIndex(spineIndex), targetOffset(targetOffset) {}

  void onStartElement(const XML_Char* rawName) {
    inBodyTextNode = false;
    pushElement(rawName);
  }

  void onEndElement() {
    inBodyTextNode = false;
    popElement();
  }

  void onCharData(const XML_Char* text, const int len) {
    if (shouldSkipText(len) || found) {
      return;
    }

    const bool atBodyLevel = bodyIdx() + 1 == static_cast<int>(stack.size());
    if (atBodyLevel && !inBodyTextNode) {
      inBodyTextNode = true;
      bodyTextNodeCount++;
      codepointsInBodyTextNode = 0;
    }

    if (isWhitespaceOnly(text, len)) {
      if (atBodyLevel) {
        codepointsInBodyTextNode += countUtf8Codepoints(text, len);
      }
      return;
    }

    const size_t visible = countVisibleBytes(text, len);
    if (totalTextBytes + visible >= targetOffset) {
      if (atBodyLevel && bodyTextNodeCount > 0) {
        // KOReader/crengine text-point semantics use codepoint offsets.
        const size_t targetVisibleByteInChunk = targetOffset - totalTextBytes;
        const size_t cpInChunk = codepointAtVisibleByte(text, len, targetVisibleByteInChunk);
        const size_t charOff = codepointsInBodyTextNode + cpInChunk;
        result =
            currentXPath(spineIndex) + "/text()[" + std::to_string(bodyTextNodeCount) + "]." + std::to_string(charOff);
      } else {
        result = currentXPath(spineIndex);
      }
      found = true;
      if (parser) {
        XML_StopParser(parser, XML_FALSE);
      }
      return;
    }

    totalTextBytes += visible;
    if (atBodyLevel) {
      codepointsInBodyTextNode += countUtf8Codepoints(text, len);
    }
  }
};

std::string makeSpineCacheKey(const std::shared_ptr<Epub>& epub, const int spineIndex) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }
  const auto spineItem = epub->getSpineItem(spineIndex);
  return epub->getCachePath() + "|" + std::to_string(spineIndex) + "|" + spineItem.href;
}

size_t getTotalTextBytesCached(const std::shared_ptr<Epub>& epub, const int spineIndex, const std::string& tmpPath) {
  static std::unordered_map<std::string, size_t> sTotalBytesBySpine;
  static std::string sCachedBookPath;

  const std::string currentBookPath = epub ? epub->getCachePath() : std::string();
  if (currentBookPath != sCachedBookPath) {
    sTotalBytesBySpine.clear();
    sCachedBookPath = currentBookPath;
  }

  const std::string key = makeSpineCacheKey(epub, spineIndex);
  if (!key.empty()) {
    const auto it = sTotalBytesBySpine.find(key);
    if (it != sTotalBytesBySpine.end()) {
      return it->second;
    }
  }

  const size_t totalTextBytes = countTotalTextBytes(tmpPath);
  if (!key.empty()) {
    sTotalBytesBySpine[key] = totalTextBytes;
  }
  return totalTextBytes;
}

}  // namespace

std::string findXPathForProgressInternal(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                         const float intraSpineProgress) {
  const std::string tmpPath = decompressToTempFile(epub, spineIndex);
  if (tmpPath.empty()) {
    return "";
  }

  const size_t totalTextBytes = getTotalTextBytesCached(epub, spineIndex, tmpPath);
  if (totalTextBytes == 0) {
    Storage.remove(tmpPath.c_str());
    const std::string base = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
    LOG_DBG("KOX", "Forward: spine=%d no text, returning base xpath", spineIndex);
    return base;
  }

  const float clamped = std::max(0.0f, std::min(1.0f, intraSpineProgress));
  const size_t targetOffset = static_cast<size_t>(clamped * static_cast<float>(totalTextBytes));

  ForwardState state(spineIndex, targetOffset);
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    Storage.remove(tmpPath.c_str());
    return "";
  }

  state.parser = parser;
  XML_SetUserData(parser, &state);
  XML_SetElementHandler(parser, parserStartCb<ForwardState>, parserEndCb<ForwardState>);
  XML_SetCharacterDataHandler(parser, parserCharCb<ForwardState>);
  XML_SetDefaultHandlerExpand(parser, parserDefaultCb<ForwardState>);
  runParse(parser, tmpPath);
  XML_ParserFree(parser);
  Storage.remove(tmpPath.c_str());

  if (state.result.empty()) {
    state.result = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  }

  LOG_DBG("KOX", "Forward: spine=%d progress=%.3f target=%zu/%zu -> %s", spineIndex, intraSpineProgress, targetOffset,
          totalTextBytes, state.result.c_str());
  return state.result;
}

}  // namespace ChapterXPathIndexerInternal
