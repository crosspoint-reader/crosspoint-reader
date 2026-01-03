#include "ParsedText.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <vector>

#include "hyphenation/HyphenationCommon.h"
#include "hyphenation/Hyphenator.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

struct HyphenSplitDecision {
  size_t byteOffset;
  uint16_t prefixWidth;
  bool appendHyphen;  // true when we must draw an extra hyphen after the prefix glyphs
};

// Verifies whether the substring ending at `offset` already contains a literal hyphen glyph, so we can avoid
// drawing a duplicate hyphen when breaking the word.
bool endsWithExplicitHyphen(const std::string& word, const size_t offset) {
  if (offset == 0 || offset > word.size()) {
    return false;
  }

  const unsigned char* base = reinterpret_cast<const unsigned char*>(word.data());
  const unsigned char* ptr = base;
  const unsigned char* target = base + offset;
  const unsigned char* lastStart = nullptr;

  while (ptr < target) {
    lastStart = ptr;
    utf8NextCodepoint(&ptr);
    if (ptr > target) {
      return false;
    }
  }

  if (!lastStart || ptr != target) {
    return false;
  }

  const unsigned char* tmp = lastStart;
  const uint32_t cp = utf8NextCodepoint(&tmp);  // decode the codepoint immediately prior to the break
  return isExplicitHyphen(cp);
}

bool chooseSplitForWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                         const EpdFontStyle style, const int availableWidth, const bool includeFallback,
                         HyphenSplitDecision* decision) {
  if (!decision || availableWidth <= 0) {
    return false;
  }

  const int hyphenWidth = renderer.getTextWidth(fontId, "-", style);

  auto offsets = Hyphenator::breakOffsets(word, includeFallback);
  if (offsets.empty()) {
    return false;
  }

  size_t chosenOffset = std::numeric_limits<size_t>::max();
  uint16_t chosenWidth = 0;
  bool chosenAppendHyphen = true;

  for (const size_t offset : offsets) {
    const bool needsInsertedHyphen = !endsWithExplicitHyphen(word, offset);
    const int budget = availableWidth - (needsInsertedHyphen ? hyphenWidth : 0);
    if (budget <= 0) {
      continue;
    }
    const std::string prefix = word.substr(0, offset);
    const int prefixWidth = renderer.getTextWidth(fontId, prefix.c_str(), style);
    if (prefixWidth <= budget) {
      chosenOffset = offset;
      chosenWidth = static_cast<uint16_t>(prefixWidth + (needsInsertedHyphen ? hyphenWidth : 0));
      chosenAppendHyphen = needsInsertedHyphen;
    } else {
      break;
    }
  }

  if (chosenOffset == std::numeric_limits<size_t>::max()) {
    return false;
  }

  decision->byteOffset = chosenOffset;
  decision->prefixWidth = chosenWidth;
  decision->appendHyphen = chosenAppendHyphen;
  return true;
}

}  // namespace

void ParsedText::addWord(std::string word, const EpdFontStyle fontStyle) {
  if (word.empty()) return;

  words.push_back(std::move(word));
  wordStyles.push_back(fontStyle);
}

// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const int horizontalMargin,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                       const bool includeLastLine) {
  if (words.empty()) {
    return;
  }

  const int pageWidth = renderer.getScreenWidth() - horizontalMargin;
  const int spaceWidth = renderer.getSpaceWidth(fontId);
  // Pre-split oversized tokens so the DP step always has feasible line candidates.
  auto wordWidths = calculateWordWidths(renderer, fontId, pageWidth);
  auto lineBreakIndices = computeLineBreaks(renderer, fontId, pageWidth, spaceWidth, wordWidths);
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, spaceWidth, wordWidths, lineBreakIndices, processLine);
  }
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId,
                                                      const int pageWidth) {
  const size_t totalWordCount = words.size();

  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(totalWordCount);

  // add em-space at the beginning of first word in paragraph to indent
  if (!extraParagraphSpacing) {
    std::string& first_word = words.front();
    first_word.insert(0, "\xe2\x80\x83");
  }

  auto wordsIt = words.begin();
  auto wordStylesIt = wordStyles.begin();

  while (wordsIt != words.end()) {
    uint16_t width = renderer.getTextWidth(fontId, wordsIt->c_str(), *wordStylesIt);

    if (width > pageWidth) {
      HyphenSplitDecision decision{};
      if (chooseSplitForWidth(renderer, fontId, *wordsIt, *wordStylesIt, pageWidth, true, &decision)) {
        const std::string originalWord = *wordsIt;
        const std::string tail = originalWord.substr(decision.byteOffset);
        if (tail.empty()) {
          continue;
        }
        std::string prefix = originalWord.substr(0, decision.byteOffset);
        if (decision.appendHyphen) {
          prefix += "-";
        }

        *wordsIt = prefix;
        auto nextWordIt = words.insert(std::next(wordsIt), tail);
        auto nextStyleIt = wordStyles.insert(std::next(wordStylesIt), *wordStylesIt);
        // Continue processing the freshly inserted tail so cascading splits still respect the limit.

        wordWidths.push_back(decision.prefixWidth);

        wordsIt = nextWordIt;
        wordStylesIt = nextStyleIt;
        continue;
      }
    }

    wordWidths.push_back(width);

    std::advance(wordsIt, 1);
    std::advance(wordStylesIt, 1);
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  const int spaceWidth, std::vector<uint16_t>& wordWidths) {
  std::vector<size_t> lineBreakIndices;
  if (words.empty()) {
    return lineBreakIndices;
  }

  if (wordWidths.empty()) {
    return lineBreakIndices;
  }

  constexpr size_t MAX_LINES = 1000;
  const auto appendLineBreak = [&](size_t index) {
    const size_t clampedIndex = std::min(index, wordWidths.size());
    lineBreakIndices.push_back(clampedIndex);
    return lineBreakIndices.size() < MAX_LINES;
  };

  const auto runDp = [&]() {
    lineBreakIndices.clear();
    const size_t totalWordCount = wordWidths.size();
    if (totalWordCount == 0) {
      return;
    }

    std::vector<int> dp(totalWordCount);
    std::vector<size_t> ans(totalWordCount);
    dp[totalWordCount - 1] = 0;
    ans[totalWordCount - 1] = totalWordCount - 1;

    for (int i = static_cast<int>(totalWordCount) - 2; i >= 0; --i) {
      int currlen = -spaceWidth;
      dp[i] = MAX_COST;

      for (size_t j = i; j < totalWordCount; ++j) {
        currlen += wordWidths[j] + spaceWidth;
        if (currlen > pageWidth) {
          break;
        }

        int cost;
        if (j == totalWordCount - 1) {
          cost = 0;
        } else {
          const int remainingSpace = pageWidth - currlen;
          const long long costLl = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];
          cost = costLl > MAX_COST ? MAX_COST : static_cast<int>(costLl);
        }

        if (cost < dp[i]) {
          dp[i] = cost;
          ans[i] = j;
        }
      }
    }

    size_t currentWordIndex = 0;
    while (currentWordIndex < totalWordCount && lineBreakIndices.size() < MAX_LINES) {
      const size_t nextBreakIndex = ans[currentWordIndex] + 1;
      if (!appendLineBreak(nextBreakIndex)) {
        break;
      }
      currentWordIndex = nextBreakIndex;
    }
  };

  const auto runHyphenatedLayout = [&]() {
    size_t currentIndex = 0;
    bool continueLayout = true;

    while (currentIndex < wordWidths.size() && continueLayout) {
      int lineWidth = 0;
      size_t wordsOnLine = 0;

      while (currentIndex < wordWidths.size()) {
        const int interWordSpace = (wordsOnLine == 0) ? 0 : spaceWidth;
        const int projectedWidth = lineWidth + interWordSpace + wordWidths[currentIndex];

        if (projectedWidth <= pageWidth) {
          lineWidth = projectedWidth;
          ++currentIndex;
          ++wordsOnLine;
          continue;
        }

        auto wordNodeIt = words.begin();
        auto styleNodeIt = wordStyles.begin();
        std::advance(wordNodeIt, currentIndex);
        std::advance(styleNodeIt, currentIndex);
        if (wordNodeIt == words.end()) {
          break;
        }

        const int availableWidth = pageWidth - lineWidth - interWordSpace;
        HyphenSplitDecision decision{};
        if (!chooseSplitForWidth(renderer, fontId, *wordNodeIt, *styleNodeIt, availableWidth, false, &decision)) {
          break;
        }

        const std::string originalWord = *wordNodeIt;
        const std::string tail = originalWord.substr(decision.byteOffset);
        if (tail.empty()) {
          break;
        }
        std::string prefix = originalWord.substr(0, decision.byteOffset);
        if (decision.appendHyphen) {
          prefix += "-";
        }

        const EpdFontStyle styleForSplit = *styleNodeIt;
        *wordNodeIt = tail;
        words.insert(wordNodeIt, prefix);
        wordStyles.insert(styleNodeIt, styleForSplit);

        const uint16_t tailWidth = renderer.getTextWidth(fontId, tail.c_str(), styleForSplit);
        wordWidths.insert(wordWidths.begin() + currentIndex, decision.prefixWidth);
        wordWidths[currentIndex + 1] = tailWidth;

        lineWidth += interWordSpace + decision.prefixWidth;
        ++currentIndex;
        ++wordsOnLine;
        break;
      }

      if (wordsOnLine == 0) {
        ++currentIndex;
      }

      continueLayout = appendLineBreak(currentIndex);
    }
  };

  if (hyphenationEnabled) {
    // Simple greedy layout with hyphenation
    runHyphenatedLayout();
  } else {
    // TeX-like optimal layout without hyphenation
    runDp();
  }

  return lineBreakIndices;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const int spaceWidth,
                             const std::vector<uint16_t>& wordWidths, const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  int lineWordWidthSum = 0;
  for (size_t idx = lastBreakAt; idx < lineBreak; ++idx) {
    lineWordWidthSum += wordWidths[idx];
  }
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  const int spareSpace = pageWidth - lineWordWidthSum;

  int spacing = spaceWidth;
  if (style == TextBlock::JUSTIFIED && !isLastLine && lineWordCount >= 2) {
    spacing = spareSpace / (lineWordCount - 1);
  }

  uint16_t xpos = 0;
  if (style == TextBlock::RIGHT_ALIGN) {
    xpos = spareSpace - (lineWordCount - 1) * spaceWidth;
  } else if (style == TextBlock::CENTER_ALIGN) {
    xpos = (spareSpace - (lineWordCount - 1) * spaceWidth) / 2;
  }

  std::list<uint16_t> lineXPos;
  for (size_t i = lastBreakAt; i < lineBreak; i++) {
    const uint16_t currentWordWidth = wordWidths[i];
    lineXPos.push_back(xpos);
    xpos += currentWordWidth + spacing;
  }

  auto wordEndIt = words.begin();
  auto wordStyleEndIt = wordStyles.begin();
  std::advance(wordEndIt, lineWordCount);
  std::advance(wordStyleEndIt, lineWordCount);

  std::list<std::string> lineWords;
  lineWords.splice(lineWords.begin(), words, words.begin(), wordEndIt);
  std::list<EpdFontStyle> lineWordStyles;
  lineWordStyles.splice(lineWordStyles.begin(), wordStyles, wordStyles.begin(), wordStyleEndIt);

  processLine(std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), style));
}