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
// 3) Emit either /text()[N].M when the cursor is at a direct text child of
//    <body>, or the bare element path otherwise.
//
// Why body-level only (and not deep nested /p[i]/span[j]/text()[k].M):
//   KOReader's crengine normalises the DOM differently than expat — it merges
//   adjacent inline elements, drops empty wrappers, and renumbers text nodes
//   inside <p>/<span>/<em>. A deep XPath we emit (e.g. /p[17]/span[1]/text()[1].26)
//   often fails to match crengine's tree, and KOReader stores a degraded
//   fallback position (start-of-wrapper-div or off-by-N text node) that
//   round-trips back to the wrong page on pull. Body-level text-point XPaths
//   have a much higher round-trip success rate even though they sacrifice
//   character-precision inside paragraphs. The Section paragraph LUT then
//   snaps the pulled position to the correct page anyway, so the precision
//   loss is invisible to users.
//
// This matches the 1.42 behavior. The pre-1.43 forward mapper only emitted
// text-point XPaths when the cursor was a direct text child of <body>; the
// 1.43 change to deep emission is the regression we're undoing here.

struct ForwardState : StackState {
  int spineIndex;
  size_t targetOffset;
  std::string result;
  bool found = false;
  XML_Parser parser = nullptr;

  // Body-level text-node bookkeeping: only counts text nodes that are direct
  // children of <body>. Inline-element text contributes to totalTextBytes via
  // the StackState base, but does not advance bodyTextNodeCount because
  // KOReader can't round-trip a deep text-node XPath reliably.
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
        // Cursor is inside a nested element. Emit the element path without a
        // text-point suffix — KOReader will treat this as a position at the
        // start of the named element, which is good enough for paragraph-level
        // accuracy. Don't emit a deep text() index here: see header comment.
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

// Paragraph-targeted forward mapper.
// Counts direct-body-child <p> elements (matching ChapterHtmlSlimParser's xpathBodyDepth guard)
// and stops at the Nth one, emitting its full-ancestry XPath.  The seek hint avoids scanning
// from byte 0 when the section LUT has a byte offset for a nearby page break.
namespace {

struct ParagraphState : StackState {
  int spineIndex;
  uint16_t targetParagraph;  // 1-based
  uint16_t paragraphCount = 0;
  std::string result;
  XML_Parser parser = nullptr;
  // When parsing from a seek offset, the DOM context (html/body ancestors) is missing from
  // the parser's perspective.  partialParse=true relaxes the bodyIdx() check and instead
  // counts any <p> at depth 0 relative to the first element seen (a heuristic that works
  // because we know we're already inside <body> in the source document).
  bool partialParse = false;
  int partialBaseDepth = -1;  // stack depth when the first element is seen in partial mode

  ParagraphState(const int spineIndex, const uint16_t targetParagraph, const uint16_t startParagraphCount,
                 const bool partialParse)
      : spineIndex(spineIndex),
        targetParagraph(targetParagraph),
        paragraphCount(startParagraphCount),
        partialParse(partialParse) {}
};

void XMLCALL paragraphStartCb(void* ud, const XML_Char* rawName, const XML_Char**) {
  auto* s = static_cast<ParagraphState*>(ud);
  s->pushElement(rawName);
  if (!s->result.empty() || s->stack.empty() || s->stack.back().tag != "p") {
    return;
  }

  bool isDirectBodyChild = false;
  if (s->partialParse) {
    // In partial mode the DOM context (html/body ancestors) is absent from the parser.
    // We record the stack depth of the first element encountered as the body-equivalent
    // depth; direct body children are one level deeper.  This only works for flat EPUBs
    // where paragraphs are direct children of <body> — for wrapped chapters the partial
    // parse will find nothing and the caller retries from byte 0 with full context.
    if (s->partialBaseDepth < 0) {
      s->partialBaseDepth = static_cast<int>(s->stack.size()) - 1;
    }
    isDirectBodyChild = (static_cast<int>(s->stack.size()) - 1 == s->partialBaseDepth);
  } else {
    const int bi = s->bodyIdx();
    isDirectBodyChild = (bi >= 0 && static_cast<int>(s->stack.size()) == bi + 2);
  }

  if (isDirectBodyChild) {
    s->paragraphCount++;
    if (s->paragraphCount >= s->targetParagraph) {
      s->result = s->currentXPath(s->spineIndex);
      if (s->parser) {
        XML_StopParser(s->parser, XML_FALSE);
      }
    }
  }
}

void XMLCALL paragraphEndCb(void* ud, const XML_Char*) { static_cast<ParagraphState*>(ud)->popElement(); }

}  // namespace

std::string findXPathForParagraphInternal(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                          const uint16_t paragraphIndex, const uint32_t seekHint,
                                          const uint16_t startParagraphCount) {
  if (!epub || paragraphIndex == 0) {
    return "";
  }

  const std::string tmpPath = decompressToTempFile(epub, spineIndex);
  if (tmpPath.empty()) {
    return "";
  }

  const bool partialParse = seekHint > 0;
  ParagraphState state(spineIndex, paragraphIndex, partialParse ? startParagraphCount : 0, partialParse);
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    Storage.remove(tmpPath.c_str());
    return "";
  }

  state.parser = parser;
  XML_SetUserData(parser, &state);
  XML_SetElementHandler(parser, paragraphStartCb, paragraphEndCb);
  // No character data handler needed — we only care about element structure.
  XML_SetDefaultHandlerExpand(parser, parserDefaultCb<ParagraphState>);

  // Use seek hint from section LUT if available — avoids scanning the whole chapter.
  // If the partial parse misses the target (e.g. the hint overshot), retry from byte 0.
  runParseFromOffset(parser, tmpPath, seekHint);

  if (state.result.empty() && seekHint > 0) {
    // Partial parse missed — reset and retry from beginning with full-document context.
    XML_ParserFree(parser);
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "XML_ParserCreate failed on retry: spine=%d p[%u] tmp=%s", spineIndex, paragraphIndex,
              tmpPath.c_str());
    } else {
      ParagraphState fullState(spineIndex, paragraphIndex, 0, false);
      fullState.parser = parser;
      XML_SetUserData(parser, &fullState);
      XML_SetElementHandler(parser, paragraphStartCb, paragraphEndCb);
      XML_SetDefaultHandlerExpand(parser, parserDefaultCb<ParagraphState>);
      runParse(parser, tmpPath);
      state.result = fullState.result;
    }
  }

  XML_ParserFree(parser);
  Storage.remove(tmpPath.c_str());

  LOG_DBG("KOX", "Paragraph: spine=%d p[%u] seekHint=%u -> %s", spineIndex, paragraphIndex, seekHint,
          state.result.empty() ? "(not found)" : state.result.c_str());
  return state.result;
}

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
