// *** BENCHMARK ONLY — do not use in production code ***
// Verbatim reimplementation of ParsedText as it existed before the Feb-2026
// performance optimisation.  Compiled only when ENABLE_PARSEDTEXT_BENCHMARK is defined.
#ifdef ENABLE_PARSEDTEXT_BENCHMARK

#include "ParsedTextLegacy.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <vector>

#include "hyphenation/Hyphenator.h"

constexpr int LEGACY_MAX_COST = std::numeric_limits<int>::max();

namespace {

constexpr char LEGACY_SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t LEGACY_SOFT_HYPHEN_BYTES = 2;

bool legacyContainsSoftHyphen(const std::string& word) {
  return word.find(LEGACY_SOFT_HYPHEN_UTF8) != std::string::npos;
}

void legacyStripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(LEGACY_SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, LEGACY_SOFT_HYPHEN_BYTES);
  }
}

uint16_t legacyMeasureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                                 const EpdFontFamily::Style style, const bool appendHyphen = false) {
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    return renderer.getSpaceWidth(fontId, style);
  }
  const bool hasSoftHyphen = legacyContainsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) {
    return renderer.getTextAdvanceX(fontId, word.c_str(), style);
  }
  std::string sanitized = word;
  if (hasSoftHyphen) {
    legacyStripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return renderer.getTextAdvanceX(fontId, sanitized.c_str(), style);
}

}  // namespace

// ---------------------------------------------------------------------------
// ParsedTextLegacy — public API
// ---------------------------------------------------------------------------

void ParsedTextLegacy::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                                const bool attachToPrevious) {
  if (word.empty()) return;

  words.push_back(std::move(word));
  EpdFontFamily::Style combinedStyle = fontStyle;
  if (underline) {
    combinedStyle = static_cast<EpdFontFamily::Style>(combinedStyle | EpdFontFamily::UNDERLINE);
  }
  wordStyles.push_back(combinedStyle);
  wordContinues.push_back(attachToPrevious);
}

void ParsedTextLegacy::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId,
                                              const uint16_t viewportWidth,
                                              const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                              const bool includeLastLine) {
  if (words.empty()) {
    return;
  }

  applyParagraphIndent();

  const int pageWidth = viewportWidth;
  const int spaceWidth = renderer.getSpaceWidth(fontId);
  auto wordWidths = calculateWordWidths(renderer, fontId);

  // *** LEGACY: copy the list into an indexed vector on every call ***
  std::vector<bool> continuesVec(wordContinues.begin(), wordContinues.end());

  std::vector<size_t> lineBreakIndices;
  if (hyphenationEnabled) {
    lineBreakIndices =
        computeHyphenatedLineBreaks(renderer, fontId, pageWidth, spaceWidth, wordWidths, continuesVec);
  } else {
    lineBreakIndices = computeLineBreaks(renderer, fontId, pageWidth, spaceWidth, wordWidths, continuesVec);
  }
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, spaceWidth, wordWidths, continuesVec, lineBreakIndices, processLine);
  }
}

// ---------------------------------------------------------------------------
// ParsedTextLegacy — private helpers
// ---------------------------------------------------------------------------

std::vector<uint16_t> ParsedTextLegacy::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  const size_t totalWordCount = words.size();

  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(totalWordCount);

  auto wordsIt = words.begin();
  auto wordStylesIt = wordStyles.begin();

  while (wordsIt != words.end()) {
    wordWidths.push_back(legacyMeasureWordWidth(renderer, fontId, *wordsIt, *wordStylesIt));
    // *** LEGACY: std::advance instead of pre-increment ***
    std::advance(wordsIt, 1);
    std::advance(wordStylesIt, 1);
  }

  return wordWidths;
}

std::vector<size_t> ParsedTextLegacy::computeLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                         const int pageWidth, const int spaceWidth,
                                                         std::vector<uint16_t>& wordWidths,
                                                         std::vector<bool>& continuesVec) {
  if (words.empty()) {
    return {};
  }

  const int firstLineIndent =
      blockStyle.textIndent > 0 && !extraParagraphSpacing &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  for (size_t i = 0; i < wordWidths.size(); ++i) {
    const int effectiveWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;
    while (wordWidths[i] > effectiveWidth) {
      // *** LEGACY: passes &continuesVec to keep the separate copy in sync ***
      if (!hyphenateWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true,
                                &continuesVec)) {
        break;
      }
    }
  }

  const size_t totalWordCount = words.size();

  std::vector<int> dp(totalWordCount);
  std::vector<size_t> ans(totalWordCount);

  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = 0;
    dp[i] = LEGACY_MAX_COST;

    const int effectivePageWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;

    for (size_t j = i; j < totalWordCount; ++j) {
      // *** LEGACY: reads from the separate continuesVec copy ***
      const int gap = j > static_cast<size_t>(i) && !continuesVec[j] ? spaceWidth : 0;
      currlen += wordWidths[j] + gap;

      if (currlen > effectivePageWidth) {
        break;
      }

      if (j + 1 < totalWordCount && continuesVec[j + 1]) {
        continue;
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;
      } else {
        const int remainingSpace = effectivePageWidth - currlen;
        const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];
        cost = cost_ll > LEGACY_MAX_COST ? LEGACY_MAX_COST : static_cast<int>(cost_ll);
      }

      if (cost < dp[i]) {
        dp[i] = cost;
        ans[i] = j;
      }
    }

    if (dp[i] == LEGACY_MAX_COST) {
      ans[i] = i;
      dp[i] = (i + 1 < static_cast<int>(totalWordCount)) ? dp[i + 1] : 0;
    }
  }

  // *** LEGACY: no reserve() on lineBreakIndices ***
  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;
    if (nextBreakIndex <= currentWordIndex) {
      nextBreakIndex = currentWordIndex + 1;
    }
    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return lineBreakIndices;
}

void ParsedTextLegacy::applyParagraphIndent() {
  if (extraParagraphSpacing || words.empty()) {
    return;
  }

  if (blockStyle.textIndentDefined) {
    // CSS text-indent set explicitly — handled in extractLine
  } else if (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left) {
    words.front().insert(0, "\xe2\x80\x83");
  }
}

std::vector<size_t> ParsedTextLegacy::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                                    const int pageWidth, const int spaceWidth,
                                                                    std::vector<uint16_t>& wordWidths,
                                                                    std::vector<bool>& continuesVec) {
  const int firstLineIndent =
      blockStyle.textIndent > 0 && !extraParagraphSpacing &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  std::vector<size_t> lineBreakIndices;
  size_t currentIndex = 0;
  bool isFirstLine = true;

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;
    const int effectivePageWidth = isFirstLine ? pageWidth - firstLineIndent : pageWidth;

    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      // *** LEGACY: reads from the separate continuesVec copy ***
      const int spacing = isFirstWord || continuesVec[currentIndex] ? 0 : spaceWidth;
      const int candidateWidth = spacing + wordWidths[currentIndex];

      if (lineWidth + candidateWidth <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;

      // *** LEGACY: passes &continuesVec ***
      if (availableWidth > 0 && hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths,
                                                     allowFallbackBreaks, &continuesVec)) {
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

    // *** LEGACY: reads from the separate continuesVec copy ***
    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() && continuesVec[currentIndex]) {
      --currentIndex;
    }

    lineBreakIndices.push_back(currentIndex);
    isFirstLine = false;
  }

  return lineBreakIndices;
}

bool ParsedTextLegacy::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth,
                                             const GfxRenderer& renderer, const int fontId,
                                             std::vector<uint16_t>& wordWidths, const bool allowFallbackBreaks,
                                             std::vector<bool>* continuesVec) {
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  // *** LEGACY: O(n) iterator walk via std::advance ***
  auto wordIt = words.begin();
  auto styleIt = wordStyles.begin();
  std::advance(wordIt, wordIndex);
  std::advance(styleIt, wordIndex);

  const std::string& word = *wordIt;
  const auto style = *styleIt;

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
    // *** LEGACY: word.substr() allocation per candidate breakpoint ***
    const int prefixWidth = legacyMeasureWordWidth(renderer, fontId, word.substr(0, offset), style, needsHyphen);
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
  wordIt->resize(chosenOffset);
  if (chosenNeedsHyphen) {
    wordIt->push_back('-');
  }

  auto insertWordIt = std::next(wordIt);
  auto insertStyleIt = std::next(styleIt);
  words.insert(insertWordIt, remainder);
  wordStyles.insert(insertStyleIt, style);

  // *** LEGACY: O(n) iterator walk into the list + dual-copy update ***
  auto continuesIt = wordContinues.begin();
  std::advance(continuesIt, wordIndex);
  const bool originalContinuedToNext = *continuesIt;
  *continuesIt = false;
  const auto insertContinuesIt = std::next(continuesIt);
  wordContinues.insert(insertContinuesIt, originalContinuedToNext);

  // *** LEGACY: also keeps the separate continuesVec in sync ***
  if (continuesVec) {
    (*continuesVec)[wordIndex] = false;
    continuesVec->insert(continuesVec->begin() + wordIndex + 1, originalContinuedToNext);
  }

  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = legacyMeasureWordWidth(renderer, fontId, remainder, style);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
}

void ParsedTextLegacy::extractLine(const size_t breakIndex, const int pageWidth, const int spaceWidth,
                                    const std::vector<uint16_t>& wordWidths, const std::vector<bool>& continuesVec,
                                    const std::vector<size_t>& lineBreakIndices,
                                    const std::function<void(std::shared_ptr<TextBlock>)>& processLine) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  const bool isFirstLine = breakIndex == 0;
  const int firstLineIndent =
      isFirstLine && blockStyle.textIndent > 0 && !extraParagraphSpacing &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    // *** LEGACY: reads from the separate continuesVec copy ***
    if (wordIdx > 0 && !continuesVec[lastBreakAt + wordIdx]) {
      actualGapCount++;
    }
  }

  const int effectivePageWidth = pageWidth - firstLineIndent;
  const int spareSpace = effectivePageWidth - lineWordWidthSum;

  int spacing = spaceWidth;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  if (blockStyle.alignment == CssTextAlign::Justify && !isLastLine && actualGapCount >= 1) {
    spacing = spareSpace / static_cast<int>(actualGapCount);
  }

  auto xpos = static_cast<uint16_t>(firstLineIndent);
  if (blockStyle.alignment == CssTextAlign::Right) {
    xpos = spareSpace - static_cast<int>(actualGapCount) * spaceWidth;
  } else if (blockStyle.alignment == CssTextAlign::Center) {
    xpos = (spareSpace - static_cast<int>(actualGapCount) * spaceWidth) / 2;
  }

  std::list<uint16_t> lineXPos;
  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    const uint16_t currentWordWidth = wordWidths[lastBreakAt + wordIdx];
    lineXPos.push_back(xpos);
    // *** LEGACY: reads from the separate continuesVec copy ***
    const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
    xpos += currentWordWidth + (nextIsContinuation ? 0 : spacing);
  }

  // *** LEGACY: three separate O(n) std::advance walks ***
  auto wordEndIt = words.begin();
  auto wordStyleEndIt = wordStyles.begin();
  auto wordContinuesEndIt = wordContinues.begin();
  std::advance(wordEndIt, lineWordCount);
  std::advance(wordStyleEndIt, lineWordCount);
  std::advance(wordContinuesEndIt, lineWordCount);

  std::list<std::string> lineWords;
  lineWords.splice(lineWords.begin(), words, words.begin(), wordEndIt);
  std::list<EpdFontFamily::Style> lineWordStyles;
  lineWordStyles.splice(lineWordStyles.begin(), wordStyles, wordStyles.begin(), wordStyleEndIt);

  // *** LEGACY: materialises a throw-away lineContinues list ***
  std::list<bool> lineContinues;
  lineContinues.splice(lineContinues.begin(), wordContinues, wordContinues.begin(), wordContinuesEndIt);

  for (auto& w : lineWords) {
    if (legacyContainsSoftHyphen(w)) {
      legacyStripSoftHyphensInPlace(w);
    }
  }

  processLine(
      std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), blockStyle));
}

#endif  // ENABLE_PARSEDTEXT_BENCHMARK
