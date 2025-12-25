#include "ParsedText.h"

#include <GfxRenderer.h>
#include "hyphenation/Hyphenator.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <vector>

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

struct HyphenSplitDecision {
  size_t byteOffset;
  uint16_t prefixWidth;
};

struct HyphenationGuard {
  size_t prefixIndex;
  size_t tailIndex;
};

bool chooseSplitForWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                         const EpdFontStyle style, const int availableWidth, const bool includeFallback,
                         HyphenSplitDecision* decision) {
  if (!decision || availableWidth <= 0) {
    return false;
  }

  const int hyphenWidth = renderer.getTextWidth(fontId, "-", style);
  const int adjustedWidth = availableWidth - hyphenWidth;
  if (adjustedWidth <= 0) {
    return false;
  }

  auto offsets = Hyphenator::breakOffsets(word, includeFallback);
  if (offsets.empty()) {
    return false;
  }

  size_t chosenOffset = std::numeric_limits<size_t>::max();
  uint16_t chosenWidth = 0;

  for (const size_t offset : offsets) {
    const std::string prefix = word.substr(0, offset);
    const int prefixWidth = renderer.getTextWidth(fontId, prefix.c_str(), style);
    if (prefixWidth <= adjustedWidth) {
      chosenOffset = offset;
      chosenWidth = static_cast<uint16_t>(prefixWidth + hyphenWidth);
    } else {
      break;
    }
  }

  if (chosenOffset == std::numeric_limits<size_t>::max()) {
    return false;
  }

  decision->byteOffset = chosenOffset;
  decision->prefixWidth = chosenWidth;
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
      HyphenSplitDecision decision;
      if (chooseSplitForWidth(renderer, fontId, *wordsIt, *wordStylesIt, pageWidth, true, &decision)) {
        const std::string originalWord = *wordsIt;
        const std::string tail = originalWord.substr(decision.byteOffset);
        if (tail.empty()) {
          continue;
        }
        const std::string prefix = originalWord.substr(0, decision.byteOffset) + "-";

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
  if (words.empty()) {
    return {};
  }

  std::vector<HyphenationGuard> guards;

  auto shiftGuardIndices = [&](size_t insertPos) {
    for (auto& guard : guards) {
      if (guard.prefixIndex >= insertPos) {
        guard.prefixIndex++;
      }
      if (guard.tailIndex >= insertPos) {
        guard.tailIndex++;
      }
    }
  };

  auto runDp = [&](std::vector<size_t>& lineBreaks) {
    const size_t totalWordCount = wordWidths.size();
 
    // DP table to store the minimum badness (cost) of lines starting at index i
    std::vector<int> dp(totalWordCount);
    // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
    std::vector<size_t> ans(totalWordCount);

    // Base Case

    dp[totalWordCount - 1] = 0;
    ans[totalWordCount - 1] = totalWordCount - 1;

    for (int i = static_cast<int>(totalWordCount) - 2; i >= 0; --i) {
      int currlen = -spaceWidth;
      dp[i] = MAX_COST;

      for (size_t j = i; j < totalWordCount; ++j) {
        // Current line length: previous width + space + current word width
        currlen += wordWidths[j] + spaceWidth;

        if (currlen > pageWidth) {
          break;
        }

        bool violatesGuard = false;
        for (const auto& guard : guards) {
          if (i <= guard.prefixIndex && j >= guard.tailIndex) {
            violatesGuard = true;
            break;
          }
        }
        if (violatesGuard) {
          continue;
        }

        int cost;
        if (j == totalWordCount - 1) {
          cost = 0;
        } else {
          const int remainingSpace = pageWidth - currlen;
          // Use long long for the square to prevent overflow
          const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];
          cost = cost_ll > MAX_COST ? MAX_COST : static_cast<int>(cost_ll);
        }

        if (cost < dp[i]) {
          dp[i] = cost;
          ans[i] = j; // j is the index of the last word in this optimal line
        }
      }
    }

    lineBreaks.clear();
    size_t currentWordIndex = 0;
    constexpr size_t MAX_LINES = 1000;

    while (currentWordIndex < totalWordCount && lineBreaks.size() < MAX_LINES) {
      const size_t nextBreakIndex = ans[currentWordIndex] + 1;
      lineBreaks.push_back(nextBreakIndex);
      currentWordIndex = nextBreakIndex;
    }
  };

  // Stores the index of the word that starts the next line (last_word_index + 1)
  std::vector<size_t> lineBreakIndices;

  while (true) {
    runDp(lineBreakIndices);

    if (!hyphenationEnabled) {
      return lineBreakIndices;
    }

    bool insertedSplit = false;
    size_t lastBreakAt = 0;

    for (size_t lineIdx = 0; lineIdx < lineBreakIndices.size(); ++lineIdx) {
      const size_t lineBreak = lineBreakIndices[lineIdx];
      const bool isLastLine = lineIdx == lineBreakIndices.size() - 1;
      const size_t lineWordCount = lineBreak - lastBreakAt;

      int lineWordWidthSum = 0;
      for (size_t idx = lastBreakAt; idx < lineBreak; ++idx) {
        lineWordWidthSum += wordWidths[idx];
      }
      lastBreakAt = lineBreak;

      if (isLastLine || lineBreak >= wordWidths.size()) {
        continue;
      }

      const size_t spacingCount = lineWordCount > 0 ? lineWordCount - 1 : 0;
      const int usedSpace = lineWordWidthSum + static_cast<int>(spacingCount) * spaceWidth;
      const int unusedWidth = pageWidth - usedSpace;
      const int spaceNeeded = lineWordCount == 0 ? 0 : spaceWidth;
      const int budgetForPrefix = unusedWidth - spaceNeeded;
      if (budgetForPrefix <= 0) {
        continue;
      }

      auto nextWordIt = words.begin();
      auto nextStyleIt = wordStyles.begin();
      std::advance(nextWordIt, lineBreak);
      std::advance(nextStyleIt, lineBreak);

      if (nextWordIt == words.end()) {
        break;
      }

      HyphenSplitDecision decision;
      if (!chooseSplitForWidth(renderer, fontId, *nextWordIt, *nextStyleIt, budgetForPrefix, false, &decision)) {
        continue;
      }

      const EpdFontStyle styleForSplit = *nextStyleIt;
      const std::string originalWord = *nextWordIt;
      const std::string prefix = originalWord.substr(0, decision.byteOffset) + "-";
      const std::string tail = originalWord.substr(decision.byteOffset);
      if (tail.empty()) {
        continue;
      }

      *nextWordIt = tail;
      words.insert(nextWordIt, prefix);
      wordStyles.insert(nextStyleIt, styleForSplit);

      const uint16_t tailWidth = renderer.getTextWidth(fontId, tail.c_str(), styleForSplit);
      wordWidths.insert(wordWidths.begin() + lineBreak, decision.prefixWidth);
      wordWidths[lineBreak + 1] = tailWidth;

      shiftGuardIndices(lineBreak);
      guards.push_back({lineBreak, lineBreak + 1});
      insertedSplit = true;
      break;
    }

    if (!insertedSplit) {
      return lineBreakIndices;
    }
  }
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const int spaceWidth,
                             const std::vector<uint16_t>& wordWidths, const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  // Calculate total word width for this line
  int lineWordWidthSum = 0;
  for (size_t i = lastBreakAt; i < lineBreak; i++) {
    lineWordWidthSum += wordWidths[i];
  }

  // Calculate spacing
  const int spareSpace = pageWidth - lineWordWidthSum;

  int spacing = spaceWidth;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;
  if (style == TextBlock::JUSTIFIED && !isLastLine && lineWordCount >= 2) {
    spacing = spareSpace / (lineWordCount - 1);
  }

  // Calculate initial x position
  uint16_t xpos = 0;
  if (style == TextBlock::RIGHT_ALIGN) {
    xpos = spareSpace - (lineWordCount - 1) * spaceWidth;
  } else if (style == TextBlock::CENTER_ALIGN) {
    xpos = (spareSpace - (lineWordCount - 1) * spaceWidth) / 2;
  }

  // Pre-calculate X positions for words
  std::list<uint16_t> lineXPos;
  for (size_t i = lastBreakAt; i < lineBreak; i++) {
    const uint16_t currentWordWidth = wordWidths[i];
    lineXPos.push_back(xpos);
    xpos += currentWordWidth + spacing;
  }

  // Iterators always start at the beginning as we are moving content with splice below
  auto wordEndIt = words.begin();
  auto wordStyleEndIt = wordStyles.begin();
  std::advance(wordEndIt, lineWordCount);
  std::advance(wordStyleEndIt, lineWordCount);

  // *** CRITICAL STEP: CONSUME DATA USING SPLICE ***
  std::list<std::string> lineWords;
  lineWords.splice(lineWords.begin(), words, words.begin(), wordEndIt);
  std::list<EpdFontStyle> lineWordStyles;
  lineWordStyles.splice(lineWordStyles.begin(), wordStyles, wordStyles.begin(), wordStyleEndIt);

  processLine(std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), style));
}