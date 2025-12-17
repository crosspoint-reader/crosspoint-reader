#include "ParsedText.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

#include "hyphenation/Hyphenator.h"

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

  const int pageWidth = renderer.getScreenWidth() - horizontalMargin;
  if (pageWidth <= 0) {
    words.clear();
    wordStyles.clear();
    return;
  }

  const int spaceWidth = renderer.getSpaceWidth(fontId);
  const bool allowIndent = !extraParagraphSpacing && (style == TextBlock::JUSTIFIED || style == TextBlock::LEFT_ALIGN);
  const int indentWidth = allowIndent ? renderer.getTextWidth(fontId, "m", REGULAR) : 0;
  const int firstLinePageWidth = allowIndent ? std::max(pageWidth - indentWidth, 0) : pageWidth;
  auto pageWidthForLine = [&](const bool isFirstLine) -> int {
    return isFirstLine ? firstLinePageWidth : pageWidth;
  };

  auto wordIt = words.begin();
  auto styleIt = wordStyles.begin();
  auto lineStartWordIt = wordIt;
  auto lineStartStyleIt = styleIt;

  int lineWidthWithSpaces = 0;
  int lineWordWidthSum = 0;
  size_t lineWordCount = 0;
  std::vector<uint16_t> lineWordWidths;
  lineWordWidths.reserve(16);

  size_t producedLines = 0;
  constexpr size_t MAX_LINES = 1000;

  auto commitLine = [&](const bool isLastLine) {
    if (lineWordCount == 0) {
      return;
    }

    const bool isFirstLine = producedLines == 0;
    const int linePageWidth = pageWidthForLine(isFirstLine);

    std::list<std::string> lineWords;
    std::list<EpdFontStyle> lineStyles;
    auto wordEndIt = wordIt;
    auto styleEndIt = styleIt;

    lineWords.splice(lineWords.begin(), words, lineStartWordIt, wordEndIt);
    lineStyles.splice(lineStyles.begin(), wordStyles, lineStartStyleIt, styleEndIt);

    const int gaps = lineWordCount > 0 ? static_cast<int>(lineWordCount - 1) : 0;
    const int baseSpaceTotal = spaceWidth * gaps;
    const int spaceBudget = linePageWidth - lineWordWidthSum;

    int spacing = spaceWidth;
    int spacingRemainder = 0;
    if (style == TextBlock::JUSTIFIED && !isLastLine && gaps > 0) {
      const int additional = std::max(0, spaceBudget - baseSpaceTotal);
      spacing = spaceWidth + (gaps > 0 ? additional / gaps : 0);
      spacingRemainder = (gaps > 0) ? additional % gaps : 0;
    }

    int renderedWidth = lineWordWidthSum;
    if (gaps > 0) {
      renderedWidth += spacing * gaps;
    }

    uint16_t xpos = 0;
    if (style == TextBlock::RIGHT_ALIGN) {
      xpos = renderedWidth < linePageWidth ? linePageWidth - renderedWidth : 0;
    } else if (style == TextBlock::CENTER_ALIGN) {
      xpos = renderedWidth < linePageWidth ? (linePageWidth - renderedWidth) / 2 : 0;
    } else if (allowIndent && isFirstLine) {
      xpos = indentWidth;
    }

    std::list<uint16_t> lineXPos;
    for (size_t idx = 0; idx < lineWordWidths.size(); ++idx) {
      lineXPos.push_back(xpos);
      xpos += lineWordWidths[idx];
      if (idx + 1 < lineWordWidths.size()) {
        int gap = spacing;
        if (spacingRemainder > 0) {
          gap += 1;
          spacingRemainder--;
        }
        xpos += gap;
      }
    }

    processLine(
        std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineStyles), style));

    producedLines++;
    lineWordWidths.clear();
    lineWordWidthSum = 0;
    lineWidthWithSpaces = 0;
    lineWordCount = 0;
    lineStartWordIt = wordIt;
    lineStartStyleIt = styleIt;
  };

  while (wordIt != words.end() && producedLines < MAX_LINES) {
    const int currentLinePageWidth = pageWidthForLine(producedLines == 0);

    if (lineWordCount == 0) {
      lineStartWordIt = wordIt;
      lineStartStyleIt = styleIt;
    }

    const int wordWidth = renderer.getTextWidth(fontId, wordIt->c_str(), *styleIt);
    const int gapWidth = (lineWordCount > 0) ? spaceWidth : 0;
    const int candidateWidth = lineWidthWithSpaces + gapWidth + wordWidth;

    if (candidateWidth <= currentLinePageWidth) {
      lineWordWidths.push_back(static_cast<uint16_t>(wordWidth));
      lineWordWidthSum += wordWidth;
      lineWidthWithSpaces = candidateWidth;
      lineWordCount++;
      ++wordIt;
      ++styleIt;
      continue;
    }

    const int availableWidth = currentLinePageWidth - lineWidthWithSpaces - gapWidth;
    if (lineWordCount > 0 && availableWidth <= 0) {
      commitLine(false);
      continue;
    }

    if (lineWordCount > 0 && availableWidth > 0) {
      HyphenationResult split;
      if (Hyphenator::splitWord(renderer, fontId, *wordIt, *styleIt, availableWidth, &split, false)) {
        *wordIt = std::move(split.head);
        auto nextWordIt = std::next(wordIt);
        auto nextStyleIt = std::next(styleIt);
        words.insert(nextWordIt, std::move(split.tail));
        wordStyles.insert(nextStyleIt, *styleIt);
        continue;
      }
    }

    if (lineWordCount == 0) {
      HyphenationResult split;
      if (Hyphenator::splitWord(renderer, fontId, *wordIt, *styleIt, currentLinePageWidth, &split, true)) {
        *wordIt = std::move(split.head);
        auto nextWordIt = std::next(wordIt);
        auto nextStyleIt = std::next(styleIt);
        words.insert(nextWordIt, std::move(split.tail));
        wordStyles.insert(nextStyleIt, *styleIt);
        continue;
      }

      lineWordWidths.push_back(static_cast<uint16_t>(wordWidth));
      lineWordWidthSum += wordWidth;
      lineWidthWithSpaces = candidateWidth;
      lineWordCount = 1;
      ++wordIt;
      ++styleIt;
      commitLine(wordIt == words.end());
      continue;
    }

    commitLine(false);
  }

  if (lineWordCount > 0 && producedLines < MAX_LINES) {
    commitLine(true);
  }

  words.clear();
  wordStyles.clear();
}