#include "TextBlock.h"

#include <EpdRenderer.h>
#include <Serialization.h>

void TextBlock::addWord(const std::string& word, const bool is_bold, const bool is_italic) {
  if (word.length() == 0) return;

  words.push_back(word);
  wordStyles.push_back((is_bold ? BOLD_SPAN : 0) | (is_italic ? ITALIC_SPAN : 0));
}

std::list<TextBlock*> TextBlock::splitIntoLines(const EpdRenderer& renderer) {
  const int totalWordCount = words.size();
  const int pageWidth = renderer.getPageWidth();
  const int spaceWidth = renderer.getSpaceWidth();

  words.shrink_to_fit();
  wordStyles.shrink_to_fit();
  wordXpos.reserve(totalWordCount);

  // measure each word
  uint16_t wordWidths[totalWordCount];
  for (int i = 0; i < words.size(); i++) {
    // measure the word
    EpdFontStyle fontStyle = REGULAR;
    if (wordStyles[i] & BOLD_SPAN) {
      if (wordStyles[i] & ITALIC_SPAN) {
        fontStyle = BOLD_ITALIC;
      } else {
        fontStyle = BOLD;
      }
    } else if (wordStyles[i] & ITALIC_SPAN) {
      fontStyle = ITALIC;
    }
    const int width = renderer.getTextWidth(words[i].c_str(), fontStyle);
    wordWidths[i] = width;
  }

  // now apply the dynamic programming algorithm to find the best line breaks
  // DP table in which dp[i] represents cost of line starting with word words[i]
  int dp[totalWordCount];

  // Array in which ans[i] store index of last word in line starting with word
  // word[i]
  size_t ans[totalWordCount];

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

  std::list<TextBlock*> lines;

  // With the line breaks calculated we can now position the words along the
  // line
  int startWord = 0;
  for (const auto lineBreak : lineBreaks) {
    const int lineWordCount = lineBreak - startWord;

    int lineWordWidthSum = 0;
    for (int i = startWord; i < lineBreak; i++) {
      lineWordWidthSum += wordWidths[i];
    }

    // Calculate spacing between words
    const uint16_t spareSpace = pageWidth - lineWordWidthSum;
    uint16_t spacing = spaceWidth;
    // evenly space words if using justified style, not the last line, and at
    // least 2 words
    if (style == JUSTIFIED && lineBreak != lineBreaks.back() && lineWordCount >= 2) {
      spacing = spareSpace / (lineWordCount - 1);
    }

    uint16_t xpos = 0;
    if (style == RIGHT_ALIGN) {
      xpos = spareSpace - (lineWordCount - 1) * spaceWidth;
    } else if (style == CENTER_ALIGN) {
      xpos = (spareSpace - (lineWordCount - 1) * spaceWidth) / 2;
    }

    for (int i = startWord; i < lineBreak; i++) {
      wordXpos[i] = xpos;
      xpos += wordWidths[i] + spacing;
    }

    std::vector<std::string> lineWords;
    std::vector<uint16_t> lineXPos;
    std::vector<uint8_t> lineWordStyles;
    lineWords.reserve(lineWordCount);
    lineXPos.reserve(lineWordCount);
    lineWordStyles.reserve(lineWordCount);

    for (int i = startWord; i < lineBreak; i++) {
      lineWords.push_back(words[i]);
      lineXPos.push_back(wordXpos[i]);
      lineWordStyles.push_back(wordStyles[i]);
    }
    const auto textLine = new TextBlock(lineWords, lineXPos, lineWordStyles, style);
    lines.push_back(textLine);
    startWord = lineBreak;
  }

  return lines;
}

void TextBlock::render(const EpdRenderer& renderer, const int x, const int y) const {
  for (int i = 0; i < words.size(); i++) {
    // render the word
    EpdFontStyle fontStyle = REGULAR;
    if (wordStyles[i] & BOLD_SPAN && wordStyles[i] & ITALIC_SPAN) {
      fontStyle = BOLD_ITALIC;
    } else if (wordStyles[i] & BOLD_SPAN) {
      fontStyle = BOLD;
    } else if (wordStyles[i] & ITALIC_SPAN) {
      fontStyle = ITALIC;
    }
    renderer.drawText(x + wordXpos[i], y, words[i].c_str(), true, fontStyle);
  }
}

void TextBlock::serialize(std::ostream& os) const {
  // words
  const uint32_t wc = words.size();
  serialization::writePod(os, wc);
  for (const auto& w : words) serialization::writeString(os, w);

  // wordXpos
  const uint32_t xc = wordXpos.size();
  serialization::writePod(os, xc);
  for (auto x : wordXpos) serialization::writePod(os, x);

  // wordStyles
  const uint32_t sc = wordStyles.size();
  serialization::writePod(os, sc);
  for (auto s : wordStyles) serialization::writePod(os, s);

  // style
  serialization::writePod(os, style);
}

TextBlock* TextBlock::deserialize(std::istream& is) {
  uint32_t wc, xc, sc;
  std::vector<std::string> words;
  std::vector<uint16_t> wordXpos;
  std::vector<uint8_t> wordStyles;
  BLOCK_STYLE style;

  // words
  serialization::readPod(is, wc);
  words.resize(wc);
  for (auto& w : words) serialization::readString(is, w);

  // wordXpos
  serialization::readPod(is, xc);
  wordXpos.resize(xc);
  for (auto& x : wordXpos) serialization::readPod(is, x);

  // wordStyles
  serialization::readPod(is, sc);
  wordStyles.resize(sc);
  for (auto& s : wordStyles) serialization::readPod(is, s);

  // style
  serialization::readPod(is, style);

  return new TextBlock(words, wordXpos, wordStyles, style);
}
