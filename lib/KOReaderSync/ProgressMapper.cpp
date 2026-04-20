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

class ParagraphStreamer final : public Print {
  size_t bytesWritten = 0;
  bool globalInTag = false;
  bool globalInEntity = false;
  enum { IDLE, SAW_LT, SAW_LT_P } pState = IDLE;
  static constexpr size_t MAX_ENTITY_SIZE = 16;
  char entityBuffer[MAX_ENTITY_SIZE] = {};
  size_t entityLen = 0;

  // Forward mode: count paragraphs at a byte offset
  size_t fwdTarget;
  int fwdResult = 0;
  bool fwdCaptured = false;

  // Reverse mode: find position of Nth paragraph + char offset
  int revParagraph;
  int revChar;
  int pCount = 0;
  bool revPFound = false;
  bool revDone = false;
  int revVisChars = 0;        // Visible chars counted WITHIN target paragraph
  size_t totalVisChars = 0;   // Total visible chars in entire file
  size_t targetVisChars = 0;  // Visible chars from start of file to target position

  void onP() {
    pCount++;
    if (!revPFound && revParagraph > 0 && pCount >= revParagraph) {
      revPFound = true;
      revVisChars = 0;
      if (revChar <= 0) {
        targetVisChars = totalVisChars;
        revDone = true;
      }
    }
  }

  void onVisibleCodepoint() {
    totalVisChars++;
    if (revPFound && !revDone) {
      revVisChars++;
      if (revVisChars >= revChar) {
        targetVisChars = totalVisChars;
        revDone = true;
      }
    }
  }

  void onVisibleText(const char* text) {
    if (!text) {
      return;
    }

    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(text);
    while (*ptr != 0) {
      utf8NextCodepoint(&ptr);
      onVisibleCodepoint();
    }
  }

  void flushEntityAsLiteral() {
    for (size_t i = 0; i < entityLen; i++) {
      onVisibleCodepoint();
    }
  }

  void finishEntity() {
    entityBuffer[entityLen] = '\0';
    const char* resolved = lookupHtmlEntity(entityBuffer, entityLen);
    if (resolved) {
      onVisibleText(resolved);
    } else {
      flushEntityAsLiteral();
    }
    globalInEntity = false;
    entityLen = 0;
  }

 public:
  explicit ParagraphStreamer(size_t targetByte) : fwdTarget(targetByte), revParagraph(0), revChar(0) {}
  ParagraphStreamer(int paragraph, int charOff) : fwdTarget(SIZE_MAX), revParagraph(paragraph), revChar(charOff) {}

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
    } else if (c == '<') {
      globalInTag = true;
    } else if (c == '>') {
      globalInTag = false;
    } else if (!globalInTag) {
      if (c == '&') {
        globalInEntity = true;
        entityBuffer[0] = '&';
        entityLen = 1;
      } else {
        const bool startsCodepoint = (c & 0xC0) != 0x80;
        if (startsCodepoint) {
          onVisibleCodepoint();
        }
      }
    }

    // Paragraph detection
    switch (pState) {
      case IDLE:
        if (c == '<') pState = SAW_LT;
        break;
      case SAW_LT:
        pState = (c == 'p' || c == 'P') ? SAW_LT_P : ((c == '<') ? SAW_LT : IDLE);
        break;
      case SAW_LT_P:
        if (c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\n' || c == '\r') onP();
        pState = (c == '<') ? SAW_LT : IDLE;
        break;
    }
    return 1;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    for (size_t i = 0; i < size; i++) write(buffer[i]);
    return size;
  }

 public:
  int paragraphCount() const { return fwdCaptured ? fwdResult : pCount; }
  size_t totalBytes() const { return bytesWritten; }
  bool found() const { return revDone || revPFound; }
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
  float intra = (pos.totalPages > 0) ? static_cast<float>(pos.pageNumber) / static_cast<float>(pos.totalPages) : 0.0f;
  result.percentage = epub->calculateProgress(pos.spineIndex, intra);
  if (pos.hasParagraphIndex && pos.paragraphIndex > 0) {
    result.xpath = ChapterXPathResolver::findXPathForParagraph(epub, pos.spineIndex, pos.paragraphIndex);
  } else {
    result.xpath = ChapterXPathResolver::findXPathForProgress(epub, pos.spineIndex, intra);
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
  const int xpathSpine = (docFrag >= 1) ? (docFrag - 1) : -1;
  if (xpathP > 0) {
    result.paragraphIndex = static_cast<uint16_t>(xpathP);
    result.hasParagraphIndex = true;
  }

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
  if (xpathP > 0) {
    ParagraphStreamer s(xpathP, xpathChar);
    if (streamSpine(epub, result.spineIndex, s) && s.found()) {
      intra = s.progress();
      LOG_DBG("PM", "XPath p[%d]+%d -> %.1f%%", xpathP, xpathChar, intra * 100);
    }
  }
  if (intra <= 0.0f) {
    const size_t bytesIn = (targetBytes > prevCum) ? (targetBytes - prevCum) : 0;
    intra = std::max(0.0f, std::min(1.0f, static_cast<float>(bytesIn) / static_cast<float>(spineSize)));
  }

  result.pageNumber = std::max(0, std::min(static_cast<int>(intra * result.totalPages), result.totalPages - 1));
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
