#include "ParsedText.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

constexpr int MAX_COST = std::numeric_limits<int>::max();

void ParsedText::addWord(std::string word, const EpdFontStyle fontStyle) {
  if (word.empty()) return;

  words.push_back(std::move(word));
  wordStyles.push_back(fontStyle);
}

// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const int horizontalMargin,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine) {
  if (words.empty()) {
    return;
  }

  const size_t totalWordCount = words.size();
  const int pageWidth = renderer.getScreenWidth() - horizontalMargin;
  const int spaceWidth = renderer.getSpaceWidth(fontId);
  // width of 1em to indent first line of paragraph if Extra Spacing is enabled
  const int indentWidth = (!extraParagraphSpacing) ? 1 * renderer.getTextWidth(fontId, "m", REGULAR) : 0;

  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(totalWordCount);

  auto wordsIt = words.begin();
  auto wordStylesIt = wordStyles.begin();

  while (wordsIt != words.end()) {
    wordWidths.push_back(renderer.getTextWidth(fontId, wordsIt->c_str(), *wordStylesIt));

    std::advance(wordsIt, 1);
    std::advance(wordStylesIt, 1);
  }

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int> dp(totalWordCount);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  std::vector<size_t> ans(totalWordCount);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = -spaceWidth + indentWidth;
    dp[i] = MAX_COST;

    for (size_t j = i; j < totalWordCount; ++j) {
      // Current line length: previous width + space + current word width
      currlen += wordWidths[j] + spaceWidth;

      if (currlen > pageWidth) {
        break;
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;  // Last line
      } else {
        const int remainingSpace = pageWidth - currlen;
        // Use long long for the square to prevent overflow
        const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      if (cost < dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1)
  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;
  constexpr size_t MAX_LINES = 1000;

  while (currentWordIndex < totalWordCount) {
    if (lineBreakIndices.size() >= MAX_LINES) {
      break;
    }

    size_t nextBreakIndex = ans[currentWordIndex] + 1;
    lineBreakIndices.push_back(nextBreakIndex);

    currentWordIndex = nextBreakIndex;
  }

  // Initialize iterators for consumption
  auto wordStartIt = words.begin();
  auto wordStyleStartIt = wordStyles.begin();
  size_t wordWidthIndex = 0;

  size_t lastBreakAt = 0;
  for (const size_t lineBreak : lineBreakIndices) {
    const size_t lineWordCount = lineBreak - lastBreakAt;

    // Calculate end iterators for the range to splice
    auto wordEndIt = wordStartIt;
    auto wordStyleEndIt = wordStyleStartIt;
    std::advance(wordEndIt, lineWordCount);
    std::advance(wordStyleEndIt, lineWordCount);

    // Calculate total word width for this line
    int lineWordWidthSum = 0;
    for (size_t i = 0; i < lineWordCount; ++i) {
      lineWordWidthSum += wordWidths[wordWidthIndex + i];
    }

    // Calculate spacing
    int spareSpace = pageWidth - lineWordWidthSum;
    if (wordWidthIndex == 0) {
      spareSpace -= indentWidth;
    }

    int spacing = spaceWidth;
    const bool isLastLine = lineBreak == totalWordCount;

    if (style == TextBlock::JUSTIFIED && !isLastLine && lineWordCount >= 2) {
      spacing = spareSpace / (lineWordCount - 1);
    }

    // Calculate initial x position
    uint16_t xpos = (wordWidthIndex == 0) ? indentWidth : 0;

    if (style == TextBlock::RIGHT_ALIGN) {
      xpos = spareSpace - (lineWordCount - 1) * spaceWidth;
    } else if (style == TextBlock::CENTER_ALIGN) {
      xpos = (spareSpace - (lineWordCount - 1) * spaceWidth) / 2;
    }

    // Pre-calculate X positions for words
    std::list<uint16_t> lineXPos;
    for (size_t i = 0; i < lineWordCount; ++i) {
      const uint16_t currentWordWidth = wordWidths[wordWidthIndex + i];
      lineXPos.push_back(xpos);
      xpos += currentWordWidth + spacing;
    }

    // *** CRITICAL STEP: CONSUME DATA USING SPLICE ***
    std::list<std::string> lineWords;
    lineWords.splice(lineWords.begin(), words, wordStartIt, wordEndIt);
    std::list<EpdFontStyle> lineWordStyles;
    lineWordStyles.splice(lineWordStyles.begin(), wordStyles, wordStyleStartIt, wordStyleEndIt);

    processLine(
        std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), style));

    // Update pointers/indices for the next line
    wordStartIt = wordEndIt;
    wordStyleStartIt = wordStyleEndIt;
    wordWidthIndex += lineWordCount;
    lastBreakAt = lineBreak;
  }
}
