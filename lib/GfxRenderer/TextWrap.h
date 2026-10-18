#pragma once

#include <cstddef>
#include <string>
#include <vector>

// GfxRenderer::wrappedText's line filling, apart from the renderer so it can be host-tested with any
// measure.
namespace textWrap {

/// The byte offsets inside a space-delimited word where a line may also break. Chinese and Japanese
/// write no spaces, so without them a sentence is one word. See GfxRenderer::setWordBreaker().
using WordBreaker = std::vector<size_t> (*)(const std::string& word);

/// Word-wrap `text` into at most `maxLines` lines no wider than `maxWidth`, breaking at spaces and at
/// `breaker`'s offsets (none when null). `width(s)` measures a candidate line; `truncate(s)` fits one
/// with an ellipsis, for a word wider than a line and for the last line when text remains.
template <typename Width, typename Truncate>
std::vector<std::string> wrapLines(const char* text, const int maxWidth, const int maxLines, const WordBreaker breaker,
                                   const Width& width, const Truncate& truncate) {
  std::vector<std::string> lines;

  if (!text || maxWidth <= 0 || maxLines <= 0) return lines;

  // The pieces a line may end after: the space-separated words, each split further at the
  // breaker's offsets. A piece split off that way joins its predecessor without a space.
  struct Piece {
    std::string text;
    bool spaceBefore;
  };
  std::vector<Piece> pieces;
  {
    const std::string all = text;
    size_t start = 0;
    while (true) {
      const size_t spacePos = all.find(' ', start);
      const std::string word = all.substr(start, spacePos == std::string::npos ? std::string::npos : spacePos - start);
      size_t from = 0;
      if (breaker) {
        for (const size_t offset : breaker(word)) {
          pieces.push_back({word.substr(from, offset - from), !pieces.empty() && from == 0});
          from = offset;
        }
      }
      pieces.push_back({word.substr(from), !pieces.empty() && from == 0});
      if (spacePos == std::string::npos) break;
      start = spacePos + 1;
    }
  }

  std::string currentLine;

  for (size_t i = 0; i < pieces.size(); ++i) {
    if (static_cast<int>(lines.size()) == maxLines - 1) {
      // Last available line: combine any word already started on this line with
      // the rest of the text, then let truncate fit it with an ellipsis.
      std::string lastContent = currentLine;
      for (size_t j = i; j < pieces.size(); ++j) {
        if (pieces[j].spaceBefore && (j > i || !lastContent.empty())) lastContent += ' ';
        lastContent += pieces[j].text;
      }
      lines.push_back(truncate(lastContent));
      return lines;
    }

    const std::string& word = pieces[i].text;
    std::string testLine = currentLine.empty() ? word : currentLine + (pieces[i].spaceBefore ? " " : "") + word;

    if (width(testLine) <= maxWidth) {
      currentLine = testLine;
    } else {
      if (!currentLine.empty()) {
        lines.push_back(currentLine);
        // If the carried-over word itself exceeds maxWidth, truncate it and
        // push it as a complete line immediately — storing it in currentLine
        // would allow a subsequent short word to be appended after the ellipsis.
        if (width(word) > maxWidth) {
          lines.push_back(truncate(word));
          currentLine.clear();
          if (static_cast<int>(lines.size()) >= maxLines) return lines;
        } else {
          currentLine = word;
        }
      } else {
        // Single word wider than maxWidth: truncate and stop to avoid complicated
        // splitting rules (different between languages). Results in an aesthetically
        // pleasing end.
        lines.push_back(truncate(word));
        return lines;
      }
    }
  }

  if (!currentLine.empty() && static_cast<int>(lines.size()) < maxLines) {
    lines.push_back(currentLine);
  }

  return lines;
}

}  // namespace textWrap
