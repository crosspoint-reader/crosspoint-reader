#include "ParsedText.h"

#include <GfxRenderer.h>
#include <ThaiShaper.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <vector>

#include "hyphenation/Hyphenator.h"

// Debug: validate Thai UTF-8 strings for corruption
// Set to 1 to enable validation in addWord
#define PARSEDTEXT_THAI_VALIDATION 1

#if PARSEDTEXT_THAI_VALIDATION
#include <Arduino.h>
static bool checkThaiWordCorruption(const std::string& word, const char* context) {
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(word.data());
  const uint8_t* end = ptr + word.size();

  while (ptr + 3 <= end) {
    if (*ptr == 0xE0 && (ptr[1] == 0x00 || ptr[2] == 0x00)) {
      Serial.printf("[PTX] %s CORRUPT @ offset %u: %02X %02X %02X in word len=%u\n",
                    context, (uint32_t)(ptr - reinterpret_cast<const uint8_t*>(word.data())),
                    ptr[0], ptr[1], ptr[2], (uint32_t)word.size());
      return true;
    }
    ptr++;
  }
  return false;
}
#endif

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Returns the rendered width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool appendHyphen = false) {
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) {
    return renderer.getTextWidth(fontId, word.c_str(), style);
  }

  std::string sanitized = word;
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return renderer.getTextWidth(fontId, sanitized.c_str(), style);
}

}  // namespace

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle) {
  if (word.empty()) return;

#if PARSEDTEXT_THAI_VALIDATION
  // Check input word BEFORE any processing
  checkThaiWordCorruption(word, "ADDWORD_INPUT");
#endif

  // Check if word contains Thai text that needs segmentation
  if (ThaiShaper::containsThai(word.c_str())) {
#if PARSEDTEXT_THAI_VALIDATION
    // Save a copy of the original input to detect if it gets corrupted during segmentation
    std::string originalCopy = word;
#endif

    // Segment Thai text into individual words for proper line breaking
    auto segmentedWords = ThaiShaper::ThaiWordBreak::segmentWords(word.c_str());

#if PARSEDTEXT_THAI_VALIDATION
    // Check if the ORIGINAL INPUT was corrupted during segmentation
    if (word != originalCopy) {
      Serial.printf("[PTX] !! ORIGINAL_CORRUPTED during segmentWords! len=%u\n", (uint32_t)word.size());
      checkThaiWordCorruption(word, "ORIGINAL_AFTER_SEG");
    }
#endif

    bool isFirst = true;
    for (auto& segment : segmentedWords) {
      if (!segment.empty()) {
#if PARSEDTEXT_THAI_VALIDATION
        // Check each segment AFTER Thai segmentation
        checkThaiWordCorruption(segment, "AFTER_SEGMENT");
#endif
        words.push_back(std::move(segment));
        wordStyles.push_back(fontStyle);
        wordNoSpaceBefore.push_back(!isFirst);  // No space before Thai cluster continuations
        isFirst = false;
      }
    }
    return;
  }

  words.push_back(std::move(word));
  wordStyles.push_back(fontStyle);
  wordNoSpaceBefore.push_back(false);  // Normal words have space before
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

  const int pageWidth = viewportWidth;
  const int spaceWidth = renderer.getSpaceWidth(fontId);
  auto wordWidths = calculateWordWidths(renderer, fontId);
  std::vector<size_t> lineBreakIndices;
  if (hyphenationEnabled) {
    // Use greedy layout that can split words mid-loop when a hyphenated prefix fits.
    lineBreakIndices = computeHyphenatedLineBreaks(renderer, fontId, pageWidth, spaceWidth, wordWidths);
  } else {
    lineBreakIndices = computeLineBreaks(renderer, fontId, pageWidth, spaceWidth, wordWidths);
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
    wordWidths.push_back(measureWordWidth(renderer, fontId, *wordsIt, *wordStylesIt));

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

  // Ensure any word that would overflow even as the first entry on a line is split using fallback hyphenation.
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    while (wordWidths[i] > pageWidth) {
      if (!hyphenateWordAtIndex(i, pageWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true)) {
        break;
      }
    }
  }

  const size_t totalWordCount = words.size();

  // Create vector copy of wordNoSpaceBefore for efficient random access
  std::vector<bool> noSpaceBeforeVec(wordNoSpaceBefore.begin(), wordNoSpaceBefore.end());

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int> dp(totalWordCount);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  std::vector<size_t> ans(totalWordCount);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = 0;
    dp[i] = MAX_COST;

    for (size_t j = i; j < totalWordCount; ++j) {
      // Current line length: previous width + space (if applicable) + current word width
      const int thisSpacing = (j == static_cast<size_t>(i) || noSpaceBeforeVec[j]) ? 0 : spaceWidth;
      currlen += wordWidths[j] + thisSpacing;

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

  if (style == TextBlock::JUSTIFIED || style == TextBlock::LEFT_ALIGN) {
    words.front().insert(0, "\xe2\x80\x83");
  }
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                            const int pageWidth, const int spaceWidth,
                                                            std::vector<uint16_t>& wordWidths) {
  std::vector<size_t> lineBreakIndices;
  size_t currentIndex = 0;

  // Create vector copy of wordNoSpaceBefore for efficient random access
  std::vector<bool> noSpaceBeforeVec(wordNoSpaceBefore.begin(), wordNoSpaceBefore.end());

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;

    // Consume as many words as possible for current line, splitting when prefixes fit
    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      const int spacing = (isFirstWord || noSpaceBeforeVec[currentIndex]) ? 0 : spaceWidth;
      const int candidateWidth = spacing + wordWidths[currentIndex];

      // Word fits on current line
      if (lineWidth + candidateWidth <= pageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      // Word would overflow — try to split based on hyphenation points
      const int availableWidth = pageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;  // Only for first word on line

      if (availableWidth > 0 &&
          hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths, allowFallbackBreaks)) {
        // Prefix now fits; append it to this line and move to next line
        // Also update noSpaceBeforeVec to match the inserted word
        noSpaceBeforeVec.insert(noSpaceBeforeVec.begin() + currentIndex + 1, false);
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      // Could not split: force at least one word per line to avoid infinite loop
      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    lineBreakIndices.push_back(currentIndex);
  }

  return lineBreakIndices;
}

// Splits words[wordIndex] into prefix (adding a hyphen only when needed) and remainder when a legal breakpoint fits the
// available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks) {
  // Guard against invalid indices or zero available width before attempting to split.
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  // Get iterators to target word, style, and no-space flag.
  auto wordIt = words.begin();
  auto styleIt = wordStyles.begin();
  auto noSpaceIt = wordNoSpaceBefore.begin();
  std::advance(wordIt, wordIndex);
  std::advance(styleIt, wordIndex);
  std::advance(noSpaceIt, wordIndex);

  const std::string& word = *wordIt;
  const auto style = *styleIt;

  // Collect candidate breakpoints (byte offsets and hyphen requirements).
  auto breakInfos = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  // Iterate over each legal breakpoint and retain the widest prefix that still fits.
  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const int prefixWidth = measureWordWidth(renderer, fontId, word.substr(0, offset), style, needsHyphen);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;  // Skip if too wide or not an improvement
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    // No hyphenation point produced a prefix that fits in the remaining space.
    return false;
  }

  // Split the word at the selected breakpoint and append a hyphen if required.
  std::string remainder = word.substr(chosenOffset);
  wordIt->resize(chosenOffset);
  if (chosenNeedsHyphen) {
    wordIt->push_back('-');
  }

  // Insert the remainder word (with matching style and no-space flag) directly after the prefix.
  auto insertWordIt = std::next(wordIt);
  auto insertStyleIt = std::next(styleIt);
  auto insertNoSpaceIt = std::next(noSpaceIt);
  words.insert(insertWordIt, remainder);
  wordStyles.insert(insertStyleIt, style);
  wordNoSpaceBefore.insert(insertNoSpaceIt, false);  // Remainder is a normal word (will be first on next line)

  // Update cached widths to reflect the new prefix/remainder pairing.
  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = measureWordWidth(renderer, fontId, remainder, style);
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

  // Count actual word gaps (excluding no-space words) for justified spacing
  auto noSpaceIt = wordNoSpaceBefore.begin();
  std::advance(noSpaceIt, lastBreakAt);
  size_t actualWordGaps = 0;
  for (size_t i = lastBreakAt; i < lineBreak; i++) {
    if (i > lastBreakAt && !*noSpaceIt) {
      actualWordGaps++;
    }
    ++noSpaceIt;
  }

  // Calculate spacing
  const int spareSpace = pageWidth - lineWordWidthSum;

  int spacing = spaceWidth;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  if (style == TextBlock::JUSTIFIED && !isLastLine && actualWordGaps >= 1) {
    spacing = spareSpace / actualWordGaps;
  }

  // Calculate initial x position (for right/center align, use actual word gaps)
  uint16_t xpos = 0;
  if (style == TextBlock::RIGHT_ALIGN) {
    xpos = spareSpace - actualWordGaps * spaceWidth;
  } else if (style == TextBlock::CENTER_ALIGN) {
    xpos = (spareSpace - actualWordGaps * spaceWidth) / 2;
  }

  // Pre-calculate X positions for words, skipping spacing for no-space words
  std::list<uint16_t> lineXPos;
  noSpaceIt = wordNoSpaceBefore.begin();
  std::advance(noSpaceIt, lastBreakAt);
  for (size_t i = lastBreakAt; i < lineBreak; i++) {
    const uint16_t currentWordWidth = wordWidths[i];
    lineXPos.push_back(xpos);
    const bool skipSpacing = (i == lastBreakAt) || *noSpaceIt;
    xpos += currentWordWidth + (skipSpacing ? 0 : spacing);
    ++noSpaceIt;
  }

  // Iterators always start at the beginning as we are moving content with splice below
  auto wordEndIt = words.begin();
  auto wordStyleEndIt = wordStyles.begin();
  auto noSpaceEndIt = wordNoSpaceBefore.begin();
  std::advance(wordEndIt, lineWordCount);
  std::advance(wordStyleEndIt, lineWordCount);
  std::advance(noSpaceEndIt, lineWordCount);

  // *** CRITICAL STEP: CONSUME DATA USING SPLICE ***
  std::list<std::string> lineWords;
  lineWords.splice(lineWords.begin(), words, words.begin(), wordEndIt);
  std::list<EpdFontFamily::Style> lineWordStyles;
  lineWordStyles.splice(lineWordStyles.begin(), wordStyles, wordStyles.begin(), wordStyleEndIt);
  std::list<bool> lineNoSpaceBefore;
  lineNoSpaceBefore.splice(lineNoSpaceBefore.begin(), wordNoSpaceBefore, wordNoSpaceBefore.begin(), noSpaceEndIt);

  for (auto& word : lineWords) {
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
  }

#if PARSEDTEXT_THAI_VALIDATION
  // Check all words just before TextBlock creation
  for (const auto& word : lineWords) {
    checkThaiWordCorruption(word, "BEFORE_TEXTBLOCK");
  }
#endif

  processLine(std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), style));
}