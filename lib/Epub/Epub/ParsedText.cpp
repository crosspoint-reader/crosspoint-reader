#include "ParsedText.h"

#include <GfxRenderer.h>
#include <Serialization.h>

#include <vector>

void ParsedText::addWord(std::string word, const bool is_bold, const bool is_italic) {
  if (word.length() == 0) return;

  words.push_back(std::move(word));
  wordStyles.push_back((is_bold ? TextBlock::BOLD_SPAN : 0) | (is_italic ? TextBlock::ITALIC_SPAN : 0));
}

// Consumes data
std::list<std::shared_ptr<TextBlock>> ParsedText::splitIntoLines(const GfxRenderer& renderer, const int fontId,
                                                                 const int horizontalMargin) {
  const int totalWordCount = words.size();
  const int pageWidth = GfxRenderer::getScreenWidth() - horizontalMargin;
  const int spaceWidth = renderer.getSpaceWidth(fontId);

  // measure each word
  std::vector<uint16_t> wordWidths;
  {
    auto wordsIt = words.begin();
    auto wordStylesIt = wordStyles.begin();
    while (wordsIt != words.end() && wordStylesIt != wordStyles.end()) {
      // measure the word
      EpdFontStyle fontStyle = REGULAR;
      if (*wordStylesIt & TextBlock::BOLD_SPAN) {
        if (*wordStylesIt & TextBlock::ITALIC_SPAN) {
          fontStyle = BOLD_ITALIC;
        } else {
          fontStyle = BOLD;
        }
      } else if (*wordStylesIt & TextBlock::ITALIC_SPAN) {
        fontStyle = ITALIC;
      }
      const int width = renderer.getTextWidth(fontId, wordsIt->c_str(), fontStyle);
      wordWidths.push_back(width);
      std::advance(wordsIt, 1);
      std::advance(wordStylesIt, 1);
    }
  }

  // Array in which ans[i] store index of last word in line starting with word
  // word[i]
  size_t ans[totalWordCount];
  {
    // now apply the dynamic programming algorithm to find the best line breaks
    // DP table in which dp[i] represents cost of line starting with word words[i]
    int dp[totalWordCount];

    // If only one word is present then only one line is required. Cost of last
    // line is zero. Hence cost of this line is zero. Ending point is also n-1 as
    // single word is present
    dp[totalWordCount - 1] = 0;
    ans[totalWordCount - 1] = totalWordCount - 1;

    // Make each word first word of line by iterating over each index in arr.
    for (int i = totalWordCount - 2; i >= 0; i--) {
      int currlen = -1;
      dp[i] = INT_MAX;

      // Variable to store possible minimum cost of line.
      int cost;

      // Keep on adding words in current line by iterating from starting word upto
      // last word in arr.
      for (int j = i; j < totalWordCount; j++) {
        // Update the width of the words in current line + the space between two
        // words.
        currlen += wordWidths[j] + spaceWidth;

        // If we're bigger than the current pagewidth then we can't add more words
        if (currlen > pageWidth) break;

        // if we've run out of words then this is last line and the cost should be
        // 0 Otherwise the cost is the sqaure of the left over space + the costs
        // of all the previous lines
        if (j == totalWordCount - 1)
          cost = 0;
        else
          cost = (pageWidth - currlen) * (pageWidth - currlen) + dp[j + 1];

        // Check if this arrangement gives minimum cost for line starting with
        // word words[i].
        if (cost < dp[i]) {
          dp[i] = cost;
          ans[i] = j;
        }
      }
    }
  }

  // We can now iterate through the answer to find the line break positions
  std::list<uint16_t> lineBreaks;
  for (size_t i = 0; i < totalWordCount;) {
    i = ans[i] + 1;
    if (i > totalWordCount) {
      break;
    }
    lineBreaks.push_back(i);
    // Text too big, just exit
    if (lineBreaks.size() > 1000) {
      break;
    }
  }

  std::list<std::shared_ptr<TextBlock>> lines;

  // With the line breaks calculated we can now position the words along the
  // line
  auto wordStartIt = words.begin();
  auto wordStyleStartIt = wordStyles.begin();
  auto wordWidthStartIt = wordWidths.begin();
  uint16_t lastBreakAt = 0;
  for (const auto lineBreak : lineBreaks) {
    const int lineWordCount = lineBreak - lastBreakAt;

    auto wordEndIt = wordStartIt;
    auto wordStyleEndIt = wordStyleStartIt;
    auto wordWidthEndIt = wordWidthStartIt;
    std::advance(wordEndIt, lineWordCount);
    std::advance(wordStyleEndIt, lineWordCount);
    std::advance(wordWidthEndIt, lineWordCount);

    int lineWordWidthSum = 0;
    for (auto it = wordWidthStartIt; it != wordWidthEndIt; std::advance(it, 1)) {
      lineWordWidthSum += *it;
    }

    // Calculate spacing between words
    const uint16_t spareSpace = pageWidth - lineWordWidthSum;
    uint16_t spacing = spaceWidth;
    // evenly space words if using justified style, not the last line, and at
    // least 2 words
    if (style == TextBlock::JUSTIFIED && lineBreak != lineBreaks.back() && lineWordCount >= 2) {
      spacing = spareSpace / (lineWordCount - 1);
    }

    uint16_t xpos = 0;
    if (style == TextBlock::RIGHT_ALIGN) {
      xpos = spareSpace - (lineWordCount - 1) * spaceWidth;
    } else if (style == TextBlock::CENTER_ALIGN) {
      xpos = (spareSpace - (lineWordCount - 1) * spaceWidth) / 2;
    }

    std::list<uint16_t> lineXPos;

    for (auto it = wordWidthStartIt; it != wordWidthEndIt; std::advance(it, 1)) {
      lineXPos.push_back(xpos);
      xpos += *it + spacing;
    }

    std::list<std::string> lineWords;
    std::list<uint8_t> lineWordStyles;
    lineWords.splice(lineWords.begin(), words, wordStartIt, wordEndIt);
    lineWordStyles.splice(lineWordStyles.begin(), wordStyles, wordStyleStartIt, wordStyleEndIt);

    lines.push_back(
        std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), style));

    wordStartIt = wordEndIt;
    wordStyleStartIt = wordStyleEndIt;
    wordWidthStartIt = wordWidthEndIt;
    lastBreakAt = lineBreak;
  }

  return lines;
}
