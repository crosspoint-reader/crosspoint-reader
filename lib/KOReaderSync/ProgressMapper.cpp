#include "ProgressMapper.h"

#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ChapterXPathResolver.h"
#include "Epub/htmlEntities.h"
#include "Utf8.h"

namespace {
int parseIndex(const std::string& xpath, const char* prefix, bool last = false) {
  const size_t prefixLen = strlen(prefix);
  const size_t pos = last ? xpath.rfind(prefix) : xpath.find(prefix);
  if (pos == std::string::npos) return -1;
  const size_t numStart = pos + prefixLen;
  const size_t numEnd = xpath.find(']', numStart);
  if (numEnd == std::string::npos || numEnd == numStart) return -1;
  int val = 0;
  for (size_t i = numStart; i < numEnd; i++) {
    if (xpath[i] < '0' || xpath[i] > '9') return -1;
    val = val * 10 + (xpath[i] - '0');
  }
  return val;
}

int parseCharOffset(const std::string& xpath) {
  const size_t textPos = xpath.rfind("text()");
  if (textPos == std::string::npos) return 0;
  const size_t dotPos = xpath.find('.', textPos);
  if (dotPos == std::string::npos || dotPos + 1 >= xpath.size()) return 0;
  int val = 0;
  for (size_t i = dotPos + 1; i < xpath.size(); i++) {
    if (xpath[i] < '0' || xpath[i] > '9') return 0;
    val = val * 10 + (xpath[i] - '0');
  }
  return val;
}

// Parse the N from text()[N] in the XPath (1-based; defaults to 1 if absent or 1).
int parseTextNodeIndex(const std::string& xpath) {
  const size_t textPos = xpath.rfind("text()[");
  if (textPos == std::string::npos) return 1;
  const size_t numStart = textPos + 7;  // strlen("text()[")
  const size_t numEnd = xpath.find(']', numStart);
  if (numEnd == std::string::npos || numEnd == numStart) return 1;
  int val = 0;
  for (size_t i = numStart; i < numEnd; i++) {
    if (xpath[i] < '0' || xpath[i] > '9') return 1;
    val = val * 10 + (xpath[i] - '0');
  }
  return val > 0 ? val : 1;
}

// Parsed representation of one step in the XPath ancestry.
struct XPathStep {
  char tag[12];      // element name, null-terminated
  int siblingIndex;  // 1-based sibling index, or 0 if unspecified (treat as 1)
};

static constexpr int MAX_XPATH_DEPTH = 16;

// Parse the XPath segment between /body/DocFragment[N]/body/ and text()[N].offset
// into an ordered sequence of steps. Returns step count, 0 on failure.
// Example input: "/body/DocFragment[1]/body/div[1]/ul/li[4]/text()[1].51"
// Fills steps with: {div,1}, {ul,1}, {li,4}
int parseXPathSteps(const std::string& xpath, XPathStep steps[MAX_XPATH_DEPTH]) {
  static const char kBodyFrag[] = "/body/DocFragment[";
  const size_t fragPos = xpath.find(kBodyFrag);
  if (fragPos == std::string::npos) return 0;
  const size_t afterBracket = xpath.find(']', fragPos + strlen(kBodyFrag));
  if (afterBracket == std::string::npos) return 0;
  static const char kBody[] = "/body/";
  if (xpath.compare(afterBracket + 1, strlen(kBody), kBody) != 0) return 0;
  size_t pos = afterBracket + 1 + strlen(kBody);

  const size_t textPos = xpath.rfind("/text()");
  if (textPos == std::string::npos || textPos <= pos) return 0;

  int count = 0;
  while (pos < textPos && count < MAX_XPATH_DEPTH) {
    const size_t slash = xpath.find('/', pos);
    const size_t segEnd = (slash < textPos) ? slash : textPos;

    XPathStep& step = steps[count];
    const size_t bracket = xpath.find('[', pos);
    const size_t nameEnd = (bracket != std::string::npos && bracket < segEnd) ? bracket : segEnd;
    const size_t nameLen = nameEnd - pos;
    if (nameLen == 0 || nameLen >= sizeof(step.tag)) return 0;
    memcpy(step.tag, xpath.c_str() + pos, nameLen);
    step.tag[nameLen] = '\0';

    if (bracket != std::string::npos && bracket < segEnd) {
      const size_t closeBracket = xpath.find(']', bracket + 1);
      if (closeBracket == std::string::npos || closeBracket > segEnd) return 0;
      int idx = 0;
      for (size_t i = bracket + 1; i < closeBracket; i++) {
        if (xpath[i] < '0' || xpath[i] > '9') return 0;
        idx = idx * 10 + (xpath[i] - '0');
      }
      step.siblingIndex = idx;
    } else {
      step.siblingIndex = 1;
    }

    count++;
    pos = (slash < textPos) ? slash + 1 : textPos;
  }
  return count;
}

class ParagraphStreamer final : public Print {
  size_t bytesWritten = 0;
  bool globalInTag = false;
  bool globalInEntity = false;
  static constexpr size_t MAX_ENTITY_SIZE = 16;
  char entityBuffer[MAX_ENTITY_SIZE] = {};
  size_t entityLen = 0;

  // Forward mode: count <p> paragraphs at a byte offset (legacy, used by generateXPath)
  size_t fwdTarget;
  int fwdResult = 0;
  bool fwdCaptured = false;

  // Reverse mode shared state
  int revChar;
  bool revPFound = false;
  bool revDone = false;
  int revVisChars = 0;
  size_t totalVisChars = 0;
  size_t targetVisChars = 0;

  // --- Legacy reverse mode (paragraph index only, no ancestry) ---
  int revParagraph = 0;
  int pCount = 0;
  int paragraphAtMatch = 0;
  int liCount = 0;
  int liCountAtMatch = 0;
  int targetTextNode = 1;
  int currentTextNode = 0;
  int paragraphHtmlDepth = -1;

  // --- Ancestry-aware reverse mode ---
  const XPathStep* steps = nullptr;
  int stepCount = 0;
  int siblingCounters[MAX_XPATH_DEPTH] = {};
  bool insideStep[MAX_XPATH_DEPTH] = {};
  int htmlDepth = 0;
  int stepEnteredAtDepth[MAX_XPATH_DEPTH] = {};

  // Tag name accumulation
  enum TagParseState { TAG_IDLE, TAG_IN_NAME, TAG_ATTRS } tagState = TAG_IDLE;
  bool tagIsClose = false;
  char tagName[12] = {};
  int tagNameLen = 0;

  int matchedDepth = 0;

  // Anchor ID capture
  static constexpr int MAX_ANCHOR_ID = 64;
  char capturedAnchorId[MAX_ANCHOR_ID] = {};
  int capturedAnchorIdLen = 0;
  bool capturingAnchorTag = false;
  enum IdScanState { ID_SCAN, ID_I, ID_D, ID_EQ, ID_IN_VALUE_D, ID_IN_VALUE_S } idState = ID_SCAN;
  bool inAttrQuote =
      false;  // true while inside a quoted attribute value (prevents '/' from being treated as self-close)
  char attrQuoteChar = 0;

  void onVisibleCodepoint() {
    totalVisChars++;
    if (revPFound && !revDone) {
      // Ancestry mode: count only while inside the fully-matched element and in the target text node.
      // Legacy mode: count only while still inside the matched paragraph and in the target text node.
      const bool inTargetNode = (stepCount > 0) ? (matchedDepth == stepCount && currentTextNode == targetTextNode)
                                                : (paragraphHtmlDepth >= 0 && currentTextNode == targetTextNode);
      if (inTargetNode) {
        revVisChars++;
        if (revVisChars >= revChar) {
          targetVisChars = totalVisChars;
          revDone = true;
        }
      }
    }
  }

  void onVisibleText(const char* text) {
    if (!text) return;
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(text);
    while (*ptr != 0) {
      utf8NextCodepoint(&ptr);
      onVisibleCodepoint();
    }
  }

  void flushEntityAsLiteral() {
    for (size_t i = 0; i < entityLen; i++) onVisibleCodepoint();
  }

  void finishEntity() {
    entityBuffer[entityLen] = '\0';
    const char* resolved = lookupHtmlEntity(entityBuffer, entityLen);
    if (resolved)
      onVisibleText(resolved);
    else
      flushEntityAsLiteral();
    globalInEntity = false;
    entityLen = 0;
  }

  void onLegacyP() {
    pCount++;
    if (!revPFound && revParagraph > 0 && pCount >= revParagraph) {
      revPFound = true;
      revVisChars = 0;
      paragraphHtmlDepth = htmlDepth;
      currentTextNode = 1;
      if (revChar <= 0 && targetTextNode <= 1) {
        targetVisChars = totalVisChars;
        revDone = true;
      }
    }
  }

  void onOpenTag() {
    htmlDepth++;

    if (stepCount == 0) {
      if (strcasecmp(tagName, "p") == 0) onLegacyP();
      return;
    }

    // Capture <a id> inside the fully-matched element even after target char is found
    if (revPFound && matchedDepth == stepCount && capturedAnchorIdLen == 0 && strcasecmp(tagName, "a") == 0) {
      capturingAnchorTag = true;
      idState = ID_SCAN;
    }

    if (revDone) return;

    if (strcasecmp(tagName, "p") == 0) pCount++;
    if (strcasecmp(tagName, "li") == 0) liCount++;

    if (matchedDepth < stepCount) {
      const XPathStep& target = steps[matchedDepth];
      if (strcasecmp(tagName, target.tag) == 0) {
        // Count only direct children of the previously matched ancestor step.
        // For step 0 any depth is valid; subsequent steps must be exactly one level deeper.
        const bool atCorrectDepth = (matchedDepth == 0) || (htmlDepth == stepEnteredAtDepth[matchedDepth - 1] + 1);
        if (!atCorrectDepth) return;
        siblingCounters[matchedDepth]++;
        if (siblingCounters[matchedDepth] == target.siblingIndex) {
          insideStep[matchedDepth] = true;
          stepEnteredAtDepth[matchedDepth] = htmlDepth;
          matchedDepth++;
          if (matchedDepth == stepCount) {
            paragraphAtMatch = pCount;
            liCountAtMatch = liCount;
            revPFound = true;
            capturedAnchorIdLen = 0;
            revVisChars = 0;
            currentTextNode = 1;  // Reset text node counter for this element
            if (revChar <= 0 && targetTextNode <= 1) {
              targetVisChars = totalVisChars;
              revDone = true;
            }
          }
        }
      }
    }
  }

  void onCloseTag() {
    // Legacy mode: each direct child element closing advances the text node index.
    if (stepCount == 0 && revPFound && !revDone && paragraphHtmlDepth >= 0 && htmlDepth == paragraphHtmlDepth + 1) {
      currentTextNode++;
      if (currentTextNode == targetTextNode && revChar <= 0) {
        targetVisChars = totalVisChars;
        revDone = true;
      }
    }
    // Legacy mode: stop tracking when the matched paragraph itself closes.
    if (stepCount == 0 && revPFound && !revDone && paragraphHtmlDepth >= 0 && htmlDepth == paragraphHtmlDepth) {
      revPFound = false;
      paragraphHtmlDepth = -1;
    }

    // Ancestry mode: advance text node when a direct child of the fully-matched element closes.
    if (stepCount > 0 && matchedDepth == stepCount && revPFound && !revDone) {
      const int elementDepth = stepEnteredAtDepth[stepCount - 1];
      if (htmlDepth == elementDepth + 1) {
        currentTextNode++;
        if (currentTextNode == targetTextNode && revChar <= 0) {
          targetVisChars = totalVisChars;
          revDone = true;
        }
      }
    }

    if (stepCount > 0 && matchedDepth > 0) {
      const int step = matchedDepth - 1;
      if (insideStep[step] && htmlDepth == stepEnteredAtDepth[step]) {
        insideStep[step] = false;
        matchedDepth--;
        // If the fully-matched element just closed without finding the target, abort.
        if (matchedDepth < stepCount && revPFound && !revDone) {
          revPFound = false;
        }
        for (int i = matchedDepth + 1; i < stepCount; i++) {
          siblingCounters[i] = 0;
          insideStep[i] = false;
          stepEnteredAtDepth[i] = -1;
        }
      }
    }
    if (htmlDepth > 0) htmlDepth--;
  }

  void processByteInTag(uint8_t c) {
    switch (tagState) {
      case TAG_IDLE:
        if (c == '/') {
          tagIsClose = true;
          tagState = TAG_IN_NAME;
        } else if (c != '!' && c != '?') {
          tagIsClose = false;
          tagName[0] = static_cast<char>(c);
          tagNameLen = 1;
          tagState = TAG_IN_NAME;
        }
        break;
      case TAG_IN_NAME:
        if (c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
          tagName[tagNameLen] = '\0';
          if (tagNameLen > 0) {
            if (tagIsClose)
              onCloseTag();
            else
              onOpenTag();
            // Self-closing open tag (<br/>). Don't double-fire for close tags (</br/>).
            if (c == '/' && !tagIsClose) onCloseTag();
          }
          tagNameLen = 0;
          tagState = (c == '>') ? TAG_IDLE : TAG_ATTRS;
        } else if (tagNameLen + 1 < static_cast<int>(sizeof(tagName))) {
          tagName[tagNameLen++] = static_cast<char>(c);
        }
        break;
      case TAG_ATTRS:
        // Track quoted attribute values so '/' inside them is not mistaken for self-closing.
        if (!inAttrQuote) {
          if (c == '"' || c == '\'') {
            inAttrQuote = true;
            attrQuoteChar = c;
          }
        } else if (c == attrQuoteChar) {
          inAttrQuote = false;
          attrQuoteChar = 0;
        }
        if (capturingAnchorTag) {
          switch (idState) {
            case ID_SCAN:
              idState = (c == 'i' || c == 'I') ? ID_I : ID_SCAN;
              break;
            case ID_I:
              idState = (c == 'd' || c == 'D') ? ID_D : ID_SCAN;
              break;
            case ID_D:
              idState = (c == '=') ? ID_EQ : ID_SCAN;
              break;
            case ID_EQ:
              if (c == '"')
                idState = ID_IN_VALUE_D;
              else if (c == '\'')
                idState = ID_IN_VALUE_S;
              break;
            case ID_IN_VALUE_D:
              if (c == '"') {
                capturedAnchorId[capturedAnchorIdLen] = '\0';
                capturingAnchorTag = false;
              } else if (capturedAnchorIdLen + 1 < MAX_ANCHOR_ID)
                capturedAnchorId[capturedAnchorIdLen++] = c;
              break;
            case ID_IN_VALUE_S:
              if (c == '\'') {
                capturedAnchorId[capturedAnchorIdLen] = '\0';
                capturingAnchorTag = false;
              } else if (capturedAnchorIdLen + 1 < MAX_ANCHOR_ID)
                capturedAnchorId[capturedAnchorIdLen++] = c;
              break;
          }
        }
        // Only treat '/' as self-closing when outside a quoted attribute value.
        if (c == '/' && !inAttrQuote) {
          onCloseTag();
          capturingAnchorTag = false;
        }
        break;
    }
  }

 public:
  explicit ParagraphStreamer(size_t targetByte) : fwdTarget(targetByte), revChar(0) {
    memset(stepEnteredAtDepth, -1, sizeof(stepEnteredAtDepth));
  }

  ParagraphStreamer(int paragraph, int charOff, int textNodeIdx = 1)
      : fwdTarget(SIZE_MAX), revChar(charOff), revParagraph(paragraph), targetTextNode(textNodeIdx) {
    memset(stepEnteredAtDepth, -1, sizeof(stepEnteredAtDepth));
  }

  ParagraphStreamer(const XPathStep* xpathSteps, int xpathStepCount, int charOff, int textNodeIdx = 1)
      : fwdTarget(SIZE_MAX),
        revChar(charOff),
        steps(xpathSteps),
        stepCount(xpathStepCount),
        targetTextNode(textNodeIdx) {
    memset(stepEnteredAtDepth, -1, sizeof(stepEnteredAtDepth));
  }

  size_t write(uint8_t c) override {
    if (!fwdCaptured && bytesWritten >= fwdTarget) {
      fwdResult = pCount;
      fwdCaptured = true;
    }
    bytesWritten++;

    if (globalInEntity) {
      if (entityLen + 1 < MAX_ENTITY_SIZE) {
        entityBuffer[entityLen++] = static_cast<char>(c);
      } else {
        flushEntityAsLiteral();
        globalInEntity = false;
        entityLen = 0;
      }
      if (globalInEntity) {
        if (c == ';') {
          finishEntity();
        } else if (c == '<' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
          flushEntityAsLiteral();
          globalInEntity = false;
          entityLen = 0;
        }
      }
      return 1;
    }

    if (c == '<') {
      globalInTag = true;
      tagState = TAG_IDLE;
      tagNameLen = 0;
      tagIsClose = false;
      capturingAnchorTag = false;
      idState = ID_SCAN;
      inAttrQuote = false;
      attrQuoteChar = 0;
    } else if (c == '>') {
      globalInTag = false;
      inAttrQuote = false;
      if (tagState == TAG_IN_NAME && tagNameLen > 0) {
        tagName[tagNameLen] = '\0';
        if (tagIsClose)
          onCloseTag();
        else
          onOpenTag();
        tagNameLen = 0;
      }
      tagState = TAG_IDLE;
    } else if (globalInTag) {
      processByteInTag(c);
    } else {
      if (c == '&') {
        globalInEntity = true;
        entityBuffer[0] = '&';
        entityLen = 1;
      } else {
        const bool startsCodepoint = (c & 0xC0) != 0x80;
        if (startsCodepoint) onVisibleCodepoint();
      }
    }
    return 1;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    for (size_t i = 0; i < size; i++) write(buffer[i]);
    return size;
  }

  int paragraphCount() const { return fwdCaptured ? fwdResult : pCount; }
  int getParagraphAtMatch() const { return paragraphAtMatch; }
  int getListItemAtMatch() const { return liCountAtMatch; }
  const char* getCapturedAnchorId() const { return capturedAnchorIdLen > 0 ? capturedAnchorId : nullptr; }
  size_t totalBytes() const { return bytesWritten; }
  bool found() const { return revDone || revPFound; }
  size_t getTotalVisChars() const { return totalVisChars; }
  size_t getTargetVisChars() const { return targetVisChars; }
  float progress() const {
    return totalVisChars > 0 ? static_cast<float>(targetVisChars) / static_cast<float>(totalVisChars) : 0.0f;
  }
};

bool streamSpine(const std::shared_ptr<Epub>& epub, int spineIndex, ParagraphStreamer& s) {
  const auto href = epub->getSpineItem(spineIndex).href;
  return !href.empty() && epub->readItemContentsToStream(href, s, 1024);
}
}  // namespace

KOReaderPosition ProgressMapper::toKOReader(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos) {
  KOReaderPosition result;
  float intra =
      (pos.totalPages > 1) ? static_cast<float>(pos.pageNumber) / static_cast<float>(pos.totalPages - 1) : 0.0f;
  result.percentage = epub->calculateProgress(pos.spineIndex, intra);
  // Progress-based XPath correctly handles both <p> and <li> positions.
  result.xpath = ChapterXPathResolver::findXPathForProgress(epub, pos.spineIndex, intra);
  // Fall back to paragraph-index lookup when progress-based resolution fails.
  if (result.xpath.empty() && pos.hasParagraphIndex && pos.paragraphIndex > 0) {
    result.xpath = ChapterXPathResolver::findXPathForParagraph(epub, pos.spineIndex, pos.paragraphIndex);
  }
  if (result.xpath.empty()) {
    result.xpath = generateXPath(epub, pos.spineIndex, intra);
  }
  LOG_DBG("PM", "-> KO: spine=%d page=%d/%d %.2f%% %s", pos.spineIndex, pos.pageNumber, pos.totalPages,
          result.percentage * 100, result.xpath.c_str());
  return result;
}

CrossPointPosition ProgressMapper::toCrossPoint(const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos,
                                                int currentSpineIndex, int totalPagesInCurrentSpine) {
  CrossPointPosition result{};
  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) return result;

  const int spineCount = epub->getSpineItemsCount();
  const float clampedPercentage = std::max(0.0f, std::min(1.0f, koPos.percentage));
  const size_t targetBytes = static_cast<size_t>(static_cast<float>(bookSize) * clampedPercentage);

  const int docFrag = parseIndex(koPos.xpath, "/body/DocFragment[");
  const int xpathP = parseIndex(koPos.xpath, "/p[", true);
  const int xpathChar = parseCharOffset(koPos.xpath);
  const int xpathTextNode = parseTextNodeIndex(koPos.xpath);
  const int xpathSpine = (docFrag >= 1) ? (docFrag - 1) : -1;

  XPathStep xpathSteps[MAX_XPATH_DEPTH];
  const int xpathStepCount = parseXPathSteps(koPos.xpath, xpathSteps);
  // Use ancestry mode whenever the XPath has a structured path (always more accurate than global counting).
  const bool useAncestry = xpathStepCount > 0;

  if (xpathSpine >= 0 && xpathSpine < spineCount) {
    result.spineIndex = xpathSpine;
  } else {
    for (int i = 0; i < spineCount; i++) {
      if (epub->getCumulativeSpineItemSize(i) >= targetBytes) {
        result.spineIndex = i;
        break;
      }
    }
  }
  if (result.spineIndex >= spineCount) return result;

  const size_t prevCum = (result.spineIndex > 0) ? epub->getCumulativeSpineItemSize(result.spineIndex - 1) : 0;
  const size_t spineSize = epub->getCumulativeSpineItemSize(result.spineIndex) - prevCum;

  if (result.spineIndex == currentSpineIndex && totalPagesInCurrentSpine > 0) {
    result.totalPages = totalPagesInCurrentSpine;
  } else if (currentSpineIndex >= 0 && currentSpineIndex < spineCount && totalPagesInCurrentSpine > 0) {
    const size_t pc = (currentSpineIndex > 0) ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
    const size_t cs = epub->getCumulativeSpineItemSize(currentSpineIndex) - pc;
    if (cs > 0)
      result.totalPages = std::max(
          1, static_cast<int>(totalPagesInCurrentSpine * static_cast<float>(spineSize) / static_cast<float>(cs)));
  }
  if (spineSize == 0 || result.totalPages == 0) return result;

  float intra = 0.0f;
  if (useAncestry) {
    ParagraphStreamer s(xpathSteps, xpathStepCount, xpathChar, xpathTextNode);
    if (streamSpine(epub, result.spineIndex, s) && s.found()) {
      intra = s.progress();
      const int pAtMatch = s.getParagraphAtMatch();
      if (pAtMatch > 0) {
        result.paragraphIndex = static_cast<uint16_t>(pAtMatch);
        result.hasParagraphIndex = true;
      }
      if (xpathStepCount > 0 && strcasecmp(xpathSteps[xpathStepCount - 1].tag, "li") == 0) {
        const int liAtMatch = s.getListItemAtMatch();
        if (liAtMatch > 0) {
          result.liIndex = static_cast<uint16_t>(liAtMatch);
          result.hasLiIndex = true;
        }
      }
      const char* anchorId = s.getCapturedAnchorId();
      if (anchorId) {
        strncpy(result.xpathAnchorId, anchorId, sizeof(result.xpathAnchorId) - 1);
      }
      LOG_DBG("PM", "XPath ancestry(%s[%d])/text()[%d]+%d -> %.1f%% (target=%zu total=%zu p~%d li~%d anchor=%s)",
              xpathSteps[xpathStepCount - 1].tag, xpathSteps[xpathStepCount - 1].siblingIndex, xpathTextNode, xpathChar,
              intra * 100, s.getTargetVisChars(), s.getTotalVisChars(), pAtMatch,
              result.hasLiIndex ? static_cast<int>(result.liIndex) : 0, anchorId ? anchorId : "none");
    }
  } else if (xpathP > 0) {
    ParagraphStreamer s(xpathP, xpathChar, xpathTextNode);
    if (streamSpine(epub, result.spineIndex, s) && s.found()) {
      intra = s.progress();
      LOG_DBG("PM", "XPath p[%d]/text()[%d]+%d -> %.1f%% (target=%zu total=%zu)", xpathP, xpathTextNode, xpathChar,
              intra * 100, s.getTargetVisChars(), s.getTotalVisChars());
    }
  }
  if (intra <= 0.0f) {
    const size_t bytesIn = (targetBytes > prevCum) ? (targetBytes - prevCum) : 0;
    intra = std::max(0.0f, std::min(1.0f, static_cast<float>(bytesIn) / static_cast<float>(spineSize)));
  }

  result.pageNumber = std::max(
      0, std::min(static_cast<int>(intra * static_cast<float>(result.totalPages - 1) + 0.5f), result.totalPages - 1));
  LOG_DBG("PM", "<- KO: %.2f%% %s -> spine=%d page=%d/%d", koPos.percentage * 100, koPos.xpath.c_str(),
          result.spineIndex, result.pageNumber, result.totalPages);
  return result;
}

std::string ProgressMapper::generateXPath(const std::shared_ptr<Epub>& epub, int spineIndex, float intra) {
  const std::string base = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  if (intra <= 0.0f) return base;

  size_t spineSize = 0;
  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty() || !epub->getItemSize(href, &spineSize) || spineSize == 0) return base;

  ParagraphStreamer s(static_cast<size_t>(spineSize * std::min(intra, 1.0f)));
  if (!streamSpine(epub, spineIndex, s)) return base;

  const int p = s.paragraphCount();
  return (p > 0) ? base + "/p[" + std::to_string(p) + "]" : base;
}
