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

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle) {
  if (word.empty()) return;

  words.push_back(std::move(word));
  wordStyles.push_back(fontStyle);
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
  ensureHyphenationFitsViewport(renderer, fontId, viewportWidth);

  const int pageWidth = viewportWidth;
  const int spaceWidth = renderer.getSpaceWidth(fontId);
  auto wordWidths = calculateWordWidths(renderer, fontId);
  std::vector<size_t> lineBreakIndices;
  if (hyphenationEnabled) {
    // Use greedy layout that can split words mid-loop when a hyphenated prefix fits.
    lineBreakIndices = computeHyphenatedLineBreaks(renderer, fontId, pageWidth, spaceWidth, wordWidths);
  } else {
    lineBreakIndices = computeLineBreaks(pageWidth, spaceWidth, wordWidths);
  }
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, spaceWidth, wordWidths, lineBreakIndices, processLine);
  }
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  const size_t totalWordCount = words.size();

  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(totalWordCount);

  auto wordsIt = words.begin();
  auto wordStylesIt = wordStyles.begin();

  while (wordsIt != words.end()) {
    wordWidths.push_back(renderer.getTextWidth(fontId, wordsIt->c_str(), *wordStylesIt));

    std::advance(wordsIt, 1);
    std::advance(wordStylesIt, 1);
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const int pageWidth, const int spaceWidth,
                                                  const std::vector<uint16_t>& wordWidths) const {
  const size_t totalWordCount = words.size();

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int> dp(totalWordCount);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  std::vector<size_t> ans(totalWordCount);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = -spaceWidth;
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

    // Handle oversized word: if no valid configuration found, force single-word line
    // This prevents cascade failure where one oversized word breaks all preceding words
    if (dp[i] == MAX_COST) {
      ans[i] = i;  // Just this word on its own line
      // Inherit cost from next word to allow subsequent words to find valid configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1)
  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return lineBreakIndices;
}

void ParsedText::applyParagraphIndent() {
  if (extraParagraphSpacing || words.empty()) {
    return;
  }

  words.front().insert(0, "\xe2\x80\x83");
}

void ParsedText::ensureHyphenationFitsViewport(const GfxRenderer& renderer, const int fontId,
                                               const int viewportWidth) {
  if (!hyphenationEnabled || viewportWidth <= 0) {
    return;
  }

  auto wordIt = words.begin();
  auto styleIt = wordStyles.begin();

  while (wordIt != words.end() && styleIt != wordStyles.end()) {
    const int wordWidth = renderer.getTextWidth(fontId, wordIt->c_str(), *styleIt);
    if (wordWidth <= viewportWidth) {
      ++wordIt;
      ++styleIt;
      continue;
    }

    auto chunks = hyphenateWordToFit(*wordIt, renderer, fontId, *styleIt, viewportWidth);
    if (chunks.empty()) {
      ++wordIt;
      ++styleIt;
      continue;
    }

    *wordIt = chunks.front();

    auto wordInsertPos = std::next(wordIt);
    auto styleInsertPos = std::next(styleIt);
    for (size_t idx = 1; idx < chunks.size(); ++idx) {
      wordInsertPos = words.insert(wordInsertPos, chunks[idx]);
      styleInsertPos = wordStyles.insert(styleInsertPos, *styleIt);
      ++wordInsertPos;
      ++styleInsertPos;
    }

    ++wordIt;
    ++styleIt;
  }
}

std::vector<std::string> ParsedText::hyphenateWordToFit(const std::string& word, const GfxRenderer& renderer,
                                                        const int fontId, const EpdFontFamily::Style style,
                                                        const int viewportWidth) const {
  std::vector<std::string> chunks;
  if (word.empty() || viewportWidth <= 0) {
    return chunks;
  }

  const int hyphenWidth = renderer.getTextWidth(fontId, "-", style);
  std::string remaining = word;

  while (!remaining.empty()) {
    const int remainingWidth = renderer.getTextWidth(fontId, remaining.c_str(), style);
    if (remainingWidth <= viewportWidth) {
      chunks.push_back(remaining);
      break;
    }

    const auto breakOffsets = Hyphenator::breakOffsets(remaining, true);
    if (breakOffsets.empty()) {
      return {};
    }

    size_t chosenOffset = 0;
    int chosenWidth = -1;
    for (const size_t offset : breakOffsets) {
      if (offset == 0 || offset >= remaining.size()) {
        continue;
      }

      const std::string candidate = remaining.substr(0, offset);
      const int candidateWidth = renderer.getTextWidth(fontId, candidate.c_str(), style) + hyphenWidth;
      if (candidateWidth > viewportWidth) {
        continue;
      }

      if (candidateWidth > chosenWidth) {
        chosenWidth = candidateWidth;
        chosenOffset = offset;
      }
    }

    if (chosenWidth < 0) {
      return {};
    }

    std::string chunk = remaining.substr(0, chosenOffset);
    chunk.push_back('-');
    chunks.push_back(std::move(chunk));
    remaining.erase(0, chosenOffset);
  }

  return chunks;
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                            const int pageWidth, const int spaceWidth,
                                                            std::vector<uint16_t>& wordWidths) {
  std::vector<size_t> lineBreakIndices;
  size_t currentIndex = 0;

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;

    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      const int spacing = isFirstWord ? 0 : spaceWidth;
      const int candidateWidth = spacing + wordWidths[currentIndex];

      if (lineWidth + candidateWidth <= pageWidth) {
        lineWidth += candidateWidth;
        currentIndex += 1;
        continue;
      }

      const int availableWidth = pageWidth - lineWidth - spacing;
      if (availableWidth > 0 && hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths)) {
        // Widths updated for the split word; retry with current index
        continue;
      }

      // Word cannot fit on current line (with or without hyphenation).
      if (currentIndex == lineStart) {
        // Force place at least one word to prevent stalling.
        lineWidth += candidateWidth;
        currentIndex += 1;
      }
      break;
    }

    lineBreakIndices.push_back(currentIndex);
  }

  return lineBreakIndices;
}

// Splits words[wordIndex] into prefix+hyphen and remainder when a legal breakpoint fits the available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths) {
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  auto wordIt = words.begin();
  auto styleIt = wordStyles.begin();
  std::advance(wordIt, wordIndex);
  std::advance(styleIt, wordIndex);

  const auto breakOffsets = Hyphenator::breakOffsets(*wordIt, true);
  if (breakOffsets.empty()) {
    return false;
  }

  const auto style = *styleIt;
  size_t chosenOffset = 0;
  int chosenWidth = -1;

  for (const size_t offset : breakOffsets) {
    if (offset == 0 || offset >= wordIt->size()) {
      continue;
    }

    std::string prefix = wordIt->substr(0, offset);
    prefix.push_back('-');
    const int prefixWidth = renderer.getTextWidth(fontId, prefix.c_str(), style);
    if (prefixWidth > availableWidth) {
      continue;
    }

    if (prefixWidth > chosenWidth) {
      chosenWidth = prefixWidth;
      chosenOffset = offset;
    }
  }

  if (chosenWidth < 0) {
    return false;
  }

  std::string remainder = wordIt->substr(chosenOffset);
  wordIt->resize(chosenOffset);
  wordIt->push_back('-');

  auto insertWordIt = std::next(wordIt);
  auto insertStyleIt = std::next(styleIt);
  words.insert(insertWordIt, remainder);
  wordStyles.insert(insertStyleIt, style);

  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = renderer.getTextWidth(fontId, remainder.c_str(), style);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
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
  std::list<EpdFontFamily::Style> lineWordStyles;
  lineWordStyles.splice(lineWordStyles.begin(), wordStyles, wordStyles.begin(), wordStyleEndIt);

  processLine(std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), style));
}