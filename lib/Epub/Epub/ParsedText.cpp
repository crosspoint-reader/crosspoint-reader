#include "ParsedText.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <vector>

#include "hyphenation/Hyphenator.h"

namespace {

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;

// Returns the first rendered codepoint of a word (skipping leading soft hyphens).
uint32_t firstCodepoint(const std::string& word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  while (true) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD) return cp;  // skip soft hyphens
  }
}

// Returns the last codepoint of a word by scanning backward for the start of the last UTF-8 sequence.
uint32_t lastCodepoint(const std::string& word) {
  if (word.empty()) return 0;
  // UTF-8 continuation bytes start with 10xxxxxx; scan backward to find the leading byte.
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) {
    --i;
  }
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + i);
  return utf8NextCodepoint(&ptr);
}

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances + kerning) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool appendHyphen = false) {
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    return renderer.getSpaceWidth(fontId, style);
  }
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) {
    return renderer.getTextAdvanceX(fontId, word.c_str(), style);
  }

  std::string sanitized = word;
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return renderer.getTextAdvanceX(fontId, sanitized.c_str(), style);
}

// Check if a word is a single CJK character (used for zero-spacing between adjacent CJK words)
bool isSingleCjkWord(const std::string& word) {
  if (word.empty()) return false;
  const auto* p = reinterpret_cast<const uint8_t*>(word.c_str());
  uint32_t cp;
  int len;
  if ((*p & 0x80) == 0) {
    cp = *p;
    len = 1;
  } else if ((*p & 0xE0) == 0xC0) {
    cp = *p & 0x1F;
    len = 2;
  } else if ((*p & 0xF0) == 0xE0) {
    cp = *p & 0x0F;
    len = 3;
  } else if ((*p & 0xF8) == 0xF0) {
    cp = *p & 0x07;
    len = 4;
  } else
    return false;
  for (int i = 1; i < len; i++) {
    if ((p[i] & 0xC0) != 0x80) return false;
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  if (static_cast<int>(word.size()) != len) return false;
  return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0x3000 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

}  // namespace

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious) {
  if (word.empty()) return;

  // Pre-allocate to match the 750-word flush threshold in characterData().
  // Without reserve(), std::vector doubles capacity on each reallocation (e.g. 512→1024),
  // requiring both old and new arrays in memory simultaneously. On a fragmented 380KB heap
  // this contiguous allocation can fail and call abort() (no C++ exceptions on ESP32).
  if (words.capacity() == 0) {
    words.reserve(800);
    wordStyles.reserve(800);
    wordContinues.reserve(800);
  }

  words.push_back(std::move(word));
  EpdFontFamily::Style combinedStyle = fontStyle;
  if (underline) {
    combinedStyle = static_cast<EpdFontFamily::Style>(combinedStyle | EpdFontFamily::UNDERLINE);
  }
  wordStyles.push_back(combinedStyle);
  wordContinues.push_back(attachToPrevious);
}

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle,
                         const VerticalTextUtils::VerticalBehavior vBehavior,
                         const bool underline, const bool attachToPrevious) {
  addWord(std::move(word), fontStyle, underline, attachToPrevious);
  if (wordVerticalBehaviors.capacity() == 0) {
    wordVerticalBehaviors.reserve(800);
  }
  wordVerticalBehaviors.push_back(vBehavior);
}

// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                       const bool includeLastLine) {
  if (words.empty()) {
    return;
  }

  // Apply fixed transforms before any per-line layout work.
  applyParagraphIndent();

  // Ensure SD card font glyph metrics are loaded before measuring word widths.
  // For flash-based fonts isSdCardFont() returns false and this block is skipped
  // entirely — no heap allocation. For SD card fonts this reads glyph metadata
  // (advanceX only, no bitmaps) for all unique codepoints in this paragraph so
  // that calculateWordWidths() can measure text without on-demand SD I/O.
  if (renderer.isSdCardFont(fontId)) {
    std::string allText;
    for (size_t i = 0; i < words.size(); i++) {
      if (i > 0) allText += ' ';
      allText += words[i];
    }
    if (hyphenationEnabled) allText += '-';
    renderer.ensureSdCardFontReady(fontId, allText.c_str());
  }

  const int pageWidth = viewportWidth;
  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);

  // CJK fallback: when firstLineIndent is ON but CSS doesn't define text-indent,
  // calculate a 2-character CJK indent width and inject it as textIndent for layout.
  // Skip when textIndent is explicitly negative (hanging indent for <li> bullets).
  if (firstLineIndent && blockStyle.textIndent == 0 && !blockStyle.textIndentDefined &&
      (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)) {
    const int cjkCharWidth = renderer.getTextWidth(fontId, "\xe5\xad\x97", EpdFontFamily::REGULAR);
    blockStyle.textIndent = static_cast<int16_t>(cjkCharWidth > 0 ? cjkCharWidth * 2 : spaceWidth * 6);
  }

  auto wordWidths = calculateWordWidths(renderer, fontId);

  // Build indexed continues vector from the parallel list for O(1) access during layout
  std::vector<bool> continuesVec(wordContinues.begin(), wordContinues.end());

  // Build CJK word flags for zero-spacing between adjacent CJK characters
  std::vector<bool> wordIsCjkVec;
  wordIsCjkVec.reserve(words.size());
  for (const auto& w : words) {
    wordIsCjkVec.push_back(isSingleCjkWord(w));
  }

  std::vector<size_t> lineBreakIndices;
  if (hyphenationEnabled) {
    lineBreakIndices =
        computeHyphenatedLineBreaks(renderer, fontId, pageWidth, spaceWidth, wordWidths, continuesVec, wordIsCjkVec);
  } else {
    lineBreakIndices =
        computeLineBreaks(renderer, fontId, pageWidth, spaceWidth, wordWidths, continuesVec, wordIsCjkVec);
  }
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, spaceWidth, wordWidths, continuesVec, wordIsCjkVec, lineBreakIndices, processLine,
                renderer, fontId);
  }

  // Remove consumed words so size() reflects only remaining words
  if (lineCount > 0) {
    const size_t consumed = lineBreakIndices[lineCount - 1];
    words.erase(words.begin(), words.begin() + consumed);
    wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
    wordContinues.erase(wordContinues.begin(), wordContinues.begin() + consumed);
    if (!wordVerticalBehaviors.empty()) {
      const size_t vbConsumed = std::min(consumed, wordVerticalBehaviors.size());
      wordVerticalBehaviors.erase(wordVerticalBehaviors.begin(), wordVerticalBehaviors.begin() + vbConsumed);
    }
  }
}

void ParsedText::layoutVerticalColumns(const GfxRenderer& renderer, const int fontId,
                                       const uint16_t columnHeight,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processColumn) {
  if (words.empty()) return;

  // Ensure SD card font metrics are loaded
  if (renderer.isSdCardFont(fontId)) {
    std::string allText;
    for (const auto& w : words) {
      allText += w;
      allText += ' ';
    }
    renderer.ensureSdCardFontReady(fontId, allText.c_str());
  }

  const int lineHeight = renderer.getLineHeight(fontId);

  // Calculate word heights for vertical layout (with 10% spacing, matching drawTextVertical)
  std::vector<uint16_t> wordHeights;
  wordHeights.reserve(words.size());
  for (size_t i = 0; i < words.size(); i++) {
    auto vb = (i < wordVerticalBehaviors.size()) ? wordVerticalBehaviors[i]
                                                  : VerticalTextUtils::VerticalBehavior::Upright;
    uint16_t baseHeight;
    switch (vb) {
      case VerticalTextUtils::VerticalBehavior::Sideways:
        baseHeight = renderer.getTextWidth(fontId, words[i].c_str(), wordStyles[i]);
        break;
      case VerticalTextUtils::VerticalBehavior::TateChuYoko:
        baseHeight = static_cast<uint16_t>(lineHeight);
        break;
      default:
        baseHeight = renderer.getTextWidth(fontId, words[i].c_str(), wordStyles[i]);
        break;
    }
    // Add 10% vertical spacing between characters (matches drawTextVertical advance)
    wordHeights.push_back(baseHeight + baseHeight / 10);
  }

  // Break into columns when cumulative height exceeds columnHeight
  size_t columnStart = 0;
  int currentY = 0;

  auto emitColumn = [&](size_t start, size_t end) {
    std::vector<std::string> colWords(std::make_move_iterator(words.begin() + start),
                                      std::make_move_iterator(words.begin() + end));
    std::vector<int16_t> colYpos;
    std::vector<int16_t> colXpos;
    std::vector<EpdFontFamily::Style> colStyles(wordStyles.begin() + start, wordStyles.begin() + end);
    const size_t count = end - start;
    colYpos.reserve(count);
    colXpos.resize(count, 0);

    int y = 0;
    for (size_t j = start; j < end; j++) {
      colYpos.push_back(static_cast<int16_t>(y));
      y += wordHeights[j];
    }

    processColumn(std::make_shared<TextBlock>(
        std::move(colWords), std::move(colXpos), std::move(colStyles),
        blockStyle, std::move(colYpos), true));
  };

  for (size_t i = 0; i < words.size(); i++) {
    if (currentY + wordHeights[i] > columnHeight && i > columnStart) {
      emitColumn(columnStart, i);
      columnStart = i;
      currentY = 0;
    }
    currentY += wordHeights[i];
  }

  // Emit remaining words as final column
  if (columnStart < words.size()) {
    emitColumn(columnStart, words.size());
  }

  // Consume all data (same pattern as layoutAndExtractLines)
  words.clear();
  wordStyles.clear();
  wordContinues.clear();
  wordVerticalBehaviors.clear();
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(words.size());

  for (size_t i = 0; i < words.size(); ++i) {
    wordWidths.push_back(measureWordWidth(renderer, fontId, words[i], wordStyles[i]));
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  const int spaceWidth, std::vector<uint16_t>& wordWidths,
                                                  std::vector<bool>& continuesVec, std::vector<bool>& wordIsCjkVec) {
  if (words.empty()) {
    return {};
  }

  // Compute first-line indent:
  // - Hanging indent (negative textIndent + textIndentDefined): always applied (e.g. <li> bullet)
  // - Positive first-line indent: requires user toggle (firstLineIndent)
  const bool isHangingIndent = blockStyle.textIndentDefined && blockStyle.textIndent < 0;
  const bool isFirstLineIndent = firstLineIndent && blockStyle.textIndent > 0;
  const int effectiveIndent =
      (isHangingIndent || isFirstLineIndent) &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  // Ensure any word that would overflow even as the first entry on a line is split using fallback hyphenation.
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    const int effectiveWidth = i == 0 ? pageWidth - effectiveIndent : pageWidth;
    while (wordWidths[i] > effectiveWidth) {
      if (!hyphenateWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true,
                                &continuesVec, &wordIsCjkVec)) {
        break;
      }
    }
  }

  // Greedy forward scan
  std::vector<size_t> lineBreakIndices;
  size_t currentIndex = 0;
  bool isFirstLine = true;

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    const int effectivePageWidth = isFirstLine ? pageWidth - effectiveIndent : pageWidth;
    int lineWidth = 0;

    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      const bool cjkAdj = !isFirstWord && currentIndex < wordIsCjkVec.size() && wordIsCjkVec[currentIndex] &&
                          wordIsCjkVec[currentIndex - 1];
      const int gap = isFirstWord || continuesVec[currentIndex] || cjkAdj ? 0 : spaceWidth;
      const int candidateWidth = gap + wordWidths[currentIndex];

      if (lineWidth + candidateWidth <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      if (currentIndex == lineStart) {
        ++currentIndex;
      }
      break;
    }

    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() && continuesVec[currentIndex]) {
      --currentIndex;
    }

    lineBreakIndices.push_back(currentIndex);
    isFirstLine = false;
  }

  return lineBreakIndices;
}

void ParsedText::applyParagraphIndent() {
  if (words.empty()) {
    return;
  }

  if (firstLineIndent || blockStyle.textIndentDefined) {
    // Indent is applied as pixel offset during layout (firstLineIndent toggle or CSS text-indent).
  } else if (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left) {
    // No indent configured - use EmSpace fallback for visual indent
    words.front().insert(0, "\xe2\x80\x83");
  }
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                            const int pageWidth, const int spaceWidth,
                                                            std::vector<uint16_t>& wordWidths,
                                                            std::vector<bool>& continuesVec,
                                                            std::vector<bool>& wordIsCjkVec) {
  const bool isHangingIndent2 = blockStyle.textIndentDefined && blockStyle.textIndent < 0;
  const bool isFirstLineIndent2 = firstLineIndent && blockStyle.textIndent > 0;
  const int effectiveIndent =
      (isHangingIndent2 || isFirstLineIndent2) &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  std::vector<size_t> lineBreakIndices;
  size_t currentIndex = 0;
  bool isFirstLine = true;

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    const int effectivePageWidth = isFirstLine ? pageWidth - effectiveIndent : pageWidth;
    int lineWidth = 0;

    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      const bool cjkAdj = !isFirstWord && currentIndex < wordIsCjkVec.size() && wordIsCjkVec[currentIndex] &&
                          wordIsCjkVec[currentIndex - 1];
      const int spacing = isFirstWord || continuesVec[currentIndex] || cjkAdj ? 0 : spaceWidth;
      const int candidateWidth = spacing + wordWidths[currentIndex];

      if (lineWidth + candidateWidth <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;

      if (availableWidth > 0 && hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths,
                                                     allowFallbackBreaks, &continuesVec, &wordIsCjkVec)) {
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() && continuesVec[currentIndex]) {
      --currentIndex;
    }

    lineBreakIndices.push_back(currentIndex);
    isFirstLine = false;
  }

  return lineBreakIndices;
}

bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks, std::vector<bool>* continuesVec,
                                      std::vector<bool>* wordIsCjkVec) {
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  const std::string& word = words[wordIndex];
  const auto style = wordStyles[wordIndex];

  auto breakInfos = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const int prefixWidth = measureWordWidth(renderer, fontId, word.substr(0, offset), style, needsHyphen);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    return false;
  }

  std::string remainder = word.substr(chosenOffset);
  words[wordIndex].resize(chosenOffset);
  if (chosenNeedsHyphen) {
    words[wordIndex].push_back('-');
  }

  words.insert(words.begin() + wordIndex + 1, remainder);
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, style);
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, false);

  if (continuesVec) {
    continuesVec->insert(continuesVec->begin() + wordIndex + 1, false);
  }

  if (wordIsCjkVec) {
    (*wordIsCjkVec)[wordIndex] = isSingleCjkWord(words[wordIndex]);
    wordIsCjkVec->insert(wordIsCjkVec->begin() + wordIndex + 1, isSingleCjkWord(remainder));
  }

  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = measureWordWidth(renderer, fontId, remainder, style);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const int spaceWidth,
                             const std::vector<uint16_t>& wordWidths, const std::vector<bool>& continuesVec,
                             const std::vector<bool>& wordIsCjkVec, const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             const GfxRenderer& renderer, const int fontId) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  const bool isFirstLine = breakIndex == 0;
  const bool isHangingIndent3 = blockStyle.textIndentDefined && blockStyle.textIndent < 0;
  const bool isFirstLineIndent3 = firstLineIndent && blockStyle.textIndent > 0;
  const int effectiveIndent =
      isFirstLine && (isHangingIndent3 || isFirstLineIndent3) &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;
  size_t nonCjkGapCount = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    if (wordIdx > 0 && !continuesVec[lastBreakAt + wordIdx]) {
      actualGapCount++;
      const bool cjkAdj = wordIsCjkVec[lastBreakAt + wordIdx] && wordIsCjkVec[lastBreakAt + wordIdx - 1];
      if (!cjkAdj) {
        nonCjkGapCount++;
      }
    }
  }

  const int effectivePageWidth = pageWidth - effectiveIndent;
  const int spareSpace = effectivePageWidth - lineWordWidthSum;

  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;
  const bool isJustified = blockStyle.alignment == CssTextAlign::Justify && !isLastLine && actualGapCount >= 1;

  int justifiedSpacing = 0;
  if (isJustified) {
    justifiedSpacing = spareSpace / static_cast<int>(actualGapCount);
  }

  auto xpos = static_cast<int16_t>(effectiveIndent);
  if (blockStyle.alignment == CssTextAlign::Right) {
    xpos = spareSpace - static_cast<int>(nonCjkGapCount) * spaceWidth;
  } else if (blockStyle.alignment == CssTextAlign::Center) {
    xpos = (spareSpace - static_cast<int>(nonCjkGapCount) * spaceWidth) / 2;
  }

  std::vector<int16_t> lineXPos;
  lineXPos.reserve(lineWordCount);

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    const uint16_t currentWordWidth = wordWidths[lastBreakAt + wordIdx];

    lineXPos.push_back(xpos);

    const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
    int gap = 0;
    if (!nextIsContinuation && wordIdx + 1 < lineWordCount) {
      if (isJustified) {
        gap = justifiedSpacing;
      } else {
        const bool nextCjkAdj = wordIsCjkVec[lastBreakAt + wordIdx] && wordIsCjkVec[lastBreakAt + wordIdx + 1];
        gap = nextCjkAdj ? 0 : spaceWidth;
      }
    }

    xpos += currentWordWidth + gap;
  }

  std::vector<std::string> lineWords(std::make_move_iterator(words.begin() + lastBreakAt),
                                     std::make_move_iterator(words.begin() + lineBreak));
  std::vector<EpdFontFamily::Style> lineWordStyles(wordStyles.begin() + lastBreakAt, wordStyles.begin() + lineBreak);

  for (auto& word : lineWords) {
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
  }

  processLine(
      std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), blockStyle));
}
