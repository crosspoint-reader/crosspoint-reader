#include "WordSelectionMode.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>

// Check if position i in text is a dash separator (hyphen, double-hyphen, en-dash, em-dash).
// Returns the number of bytes consumed by the dash (0 if not a dash).
static int dashLen(const char* text, int i, int textLen) {
  auto b = [](const char* s, int idx) { return static_cast<unsigned char>(s[idx]); };
  // UTF-8 en-dash U+2013: E2 80 93, em-dash U+2014: E2 80 94
  if (i + 2 < textLen && b(text, i) == 0xE2 && b(text, i + 1) == 0x80 &&
      (b(text, i + 2) == 0x93 || b(text, i + 2) == 0x94)) {
    return 3;
  }
  // Double-hyphen "--" (used as em-dash in plain text)
  if (i + 1 < textLen && text[i] == '-' && text[i + 1] == '-') {
    return 2;
  }
  // Single hyphen
  if (text[i] == '-') {
    return 1;
  }
  return 0;
}

// Find dash-separated components in text. Returns total component count.
// If targetIdx >= 0, sets outStart/outLen to that component's position in text.
static int findDashComponent(const char* text, int textLen, int targetIdx, const char** outStart, int* outLen) {
  int count = 0;
  const char* compStart = nullptr;
  bool inComp = false;
  int i = 0;
  while (i <= textLen) {
    int dl = (i < textLen) ? dashLen(text, i, textLen) : 0;
    if (dl > 0 || i >= textLen) {
      if (inComp) {
        if (count == targetIdx && outStart && outLen) {
          *outStart = compStart;
          *outLen = static_cast<int>(text + i - compStart);
        }
        count++;
      }
      inComp = false;
      i += (dl > 0) ? dl : 1;
    } else {
      if (!inComp) compStart = text + i;
      inComp = true;
      i++;
    }
  }
  return count;
}

void WordSelectionMode::extractWords(const Page& page, const GfxRenderer& renderer, int fontId, int marginLeft,
                                     int marginTop) {
  wordCount = 0;
  arenaUsed = 0;
  lineCount = 0;

  const int lineHeight = renderer.getLineHeight(fontId);
  int16_t lastY = -1;

  bool exhausted = false;

  for (const auto& element : page.elements) {
    if (exhausted) break;
    if (element->getTag() != TAG_PageLine) continue;

    const auto& pageLine = static_cast<const PageLine&>(*element);
    const auto& block = pageLine.getBlock();
    const auto& wordTexts = block->getWords();
    const auto& xPositions = block->getWordXpos();
    const auto& styles = block->getWordStyles();

    if (wordTexts.empty()) continue;

    const int16_t lineY = static_cast<int16_t>(pageLine.yPos + marginTop);
    bool lineRecorded = (lineY == lastY);

    const size_t wc = wordTexts.size();
    for (size_t i = 0; i < wc; ++i) {
      if (wordCount >= MAX_WORDS || arenaUsed + static_cast<int>(wordTexts[i].size()) + 1 > ARENA_SIZE) {
        exhausted = true;
        break;
      }

      // Record new line only when we know we have at least one word to add
      if (!lineRecorded) {
        if (lineCount >= MAX_LINES) {
          exhausted = true;
          break;
        }
        lineStarts[lineCount++] = wordCount;
        lastY = lineY;
        lineRecorded = true;
      }

      const auto& text = wordTexts[i];

      // Copy word text into arena
      WordInfo& w = words[wordCount];
      w.textOffset = static_cast<int16_t>(arenaUsed);
      w.textLen = static_cast<int16_t>(text.size());
      memcpy(arena + arenaUsed, text.c_str(), text.size());
      arena[arenaUsed + text.size()] = '\0';
      arenaUsed += static_cast<int>(text.size()) + 1;

      w.screenX = static_cast<int16_t>(xPositions[i] + marginLeft);
      w.screenY = lineY;
      w.height = static_cast<int16_t>(lineHeight);
      w.style = styles[i];

      // Compute width from adjacent x-positions to avoid expensive getTextWidth calls.
      // Only the last word per TextBlock needs a measured width.
      if (i + 1 < wc) {
        w.width = static_cast<int16_t>(xPositions[i + 1] - xPositions[i]);
      } else {
        w.width = static_cast<int16_t>(renderer.getTextWidth(fontId, text.c_str(), w.style));
      }

      wordCount++;
    }
  }

  LOG_DBG("WSM", "Extracted %d words across %d lines (arena %d/%d bytes)", wordCount, lineCount, arenaUsed, ARENA_SIZE);
}

void WordSelectionMode::clear() {
  wordCount = 0;
  arenaUsed = 0;
  lineCount = 0;
  active = false;
  subSelectionActive = false;
}

void WordSelectionMode::enter() {
  if (wordCount == 0 || lineCount == 0) return;
  active = true;
  subSelectionActive = false;
  // Start cursor at middle line, first word
  cursorLine = lineCount / 2;
  cursorWord = 0;
}

void WordSelectionMode::exit() {
  active = false;
  subSelectionActive = false;
}

int WordSelectionMode::getGlobalWordIndex() const {
  if (cursorLine < 0 || cursorLine >= lineCount) return -1;
  return lineStarts[cursorLine] + cursorWord;
}

int WordSelectionMode::getLineWordCount(int line) const {
  if (line < 0 || line >= lineCount) return 0;
  const int start = lineStarts[line];
  const int end = (line + 1 < lineCount) ? lineStarts[line + 1] : wordCount;
  return end - start;
}

void WordSelectionMode::moveUp() {
  if (cursorLine <= 0) return;
  cursorLine--;
  const int count = getLineWordCount(cursorLine);
  if (cursorWord >= count) cursorWord = count - 1;
  subSelectionActive = false;
}

void WordSelectionMode::moveDown() {
  if (cursorLine >= lineCount - 1) return;
  cursorLine++;
  const int count = getLineWordCount(cursorLine);
  if (cursorWord >= count) cursorWord = count - 1;
  subSelectionActive = false;
}

void WordSelectionMode::moveLeft() {
  if (cursorWord > 0) {
    cursorWord--;
  } else if (cursorLine > 0) {
    // Wrap to end of previous line
    cursorLine--;
    cursorWord = getLineWordCount(cursorLine) - 1;
  }
  subSelectionActive = false;
}

void WordSelectionMode::moveRight() {
  const int count = getLineWordCount(cursorLine);
  if (cursorWord < count - 1) {
    cursorWord++;
  } else if (cursorLine < lineCount - 1) {
    // Wrap to start of next line
    cursorLine++;
    cursorWord = 0;
  }
  subSelectionActive = false;
}

bool WordSelectionMode::getSelectedWord(char* outBuf, int outSize) const {
  const int idx = getGlobalWordIndex();
  if (idx < 0 || idx >= wordCount || outSize <= 0) return false;

  const WordInfo& w = words[idx];
  const int len = std::min(static_cast<int>(w.textLen), outSize - 1);
  memcpy(outBuf, arena + w.textOffset, len);
  outBuf[len] = '\0';
  return true;
}

void WordSelectionMode::renderHighlight(GfxRenderer& renderer, int fontId) const {
  const int idx = getGlobalWordIndex();
  if (idx < 0 || idx >= wordCount) return;

  const WordInfo& w = words[idx];

  if (subSelectionActive) {
    // Highlight only the sub-selected component
    char fullWord[64];
    const int fullLen = std::min(static_cast<int>(w.textLen), static_cast<int>(sizeof(fullWord) - 1));
    memcpy(fullWord, arena + w.textOffset, fullLen);
    fullWord[fullLen] = '\0';

    const char* compStart = nullptr;
    int compLen = 0;
    if (findDashComponent(fullWord, fullLen, subSelectIndex, &compStart, &compLen) <= subSelectIndex) return;

    // Measure X offset to the component start
    char prefix[64];
    const int prefixLen = static_cast<int>(compStart - fullWord);
    memcpy(prefix, fullWord, prefixLen);
    prefix[prefixLen] = '\0';
    const int prefixWidth = (prefixLen > 0) ? renderer.getTextWidth(fontId, prefix, w.style) : 0;

    char comp[64];
    memcpy(comp, compStart, compLen);
    comp[compLen] = '\0';
    const int compWidth = renderer.getTextWidth(fontId, comp, w.style);

    const int hlX = w.screenX + prefixWidth;
    renderer.fillRect(hlX, w.screenY, compWidth, w.height, true);
    renderer.drawText(fontId, hlX, w.screenY, comp, false, w.style);
  } else {
    // Highlight the entire word — re-measure to exclude trailing space
    char wordBuf[64];
    const int len = std::min(static_cast<int>(w.textLen), static_cast<int>(sizeof(wordBuf) - 1));
    memcpy(wordBuf, arena + w.textOffset, len);
    wordBuf[len] = '\0';
    const int measuredWidth = renderer.getTextWidth(fontId, wordBuf, w.style);
    renderer.fillRect(w.screenX, w.screenY, measuredWidth, w.height, true);
    renderer.drawText(fontId, w.screenX, w.screenY, wordBuf, false, w.style);
  }
}

bool WordSelectionMode::isHyphenated() const {
  const int idx = getGlobalWordIndex();
  if (idx < 0 || idx >= wordCount) return false;
  const WordInfo& w = words[idx];
  return findDashComponent(arena + w.textOffset, w.textLen, -1, nullptr, nullptr) >= 2;
}

void WordSelectionMode::enterSubSelection() {
  if (!isHyphenated()) return;
  const int idx = getGlobalWordIndex();
  if (idx < 0 || idx >= wordCount) return;

  const WordInfo& w = words[idx];
  subSelectCount = findDashComponent(arena + w.textOffset, w.textLen, -1, nullptr, nullptr);
  if (subSelectCount <= 1) return;

  subSelectionActive = true;
  subSelectIndex = 0;
}

void WordSelectionMode::exitSubSelection() { subSelectionActive = false; }

bool WordSelectionMode::subSelectLeft() {
  if (subSelectIndex > 0) {
    subSelectIndex--;
    return true;
  }
  return false;
}

bool WordSelectionMode::subSelectRight() {
  if (subSelectIndex < subSelectCount - 1) {
    subSelectIndex++;
    return true;
  }
  return false;
}

bool WordSelectionMode::getSubSelectedWord(char* outBuf, int outSize) const {
  if (!subSelectionActive || outSize <= 0) return false;

  const int idx = getGlobalWordIndex();
  if (idx < 0 || idx >= wordCount) return false;

  const WordInfo& w = words[idx];
  const char* compStart = nullptr;
  int compLen = 0;
  if (findDashComponent(arena + w.textOffset, w.textLen, subSelectIndex, &compStart, &compLen) <= subSelectIndex) {
    return false;
  }
  const int len = std::min(compLen, outSize - 1);
  memcpy(outBuf, compStart, len);
  outBuf[len] = '\0';
  return true;
}
