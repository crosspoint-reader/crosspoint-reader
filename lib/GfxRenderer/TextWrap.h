#pragma once

#include <cstddef>
#include <cstring>
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

  // One word at a time, and only as far as the lines go: text can be a 2 KB book summary, and
  // splitting all of it up front cost a piece per CJK character, tens of KB of heap at once.
  const size_t length = std::strlen(text);
  size_t next = 0;  // where the next word starts
  std::string currentLine;

  while (next < length) {
    const char* space = static_cast<const char*>(std::memchr(text + next, ' ', length - next));
    const size_t wordStart = next;
    const std::string wordText(text + wordStart, (space ? static_cast<size_t>(space - text) : length) - wordStart);
    next = space ? static_cast<size_t>(space - text) + 1 : length;

    // The word's pieces: split at the breaker's offsets, they join without a space.
    const std::vector<size_t> breaks = breaker ? breaker(wordText) : std::vector<size_t>{};
    size_t from = 0;
    for (size_t b = 0; b <= breaks.size(); ++b) {
      const bool firstPiece = from == 0;
      if (static_cast<int>(lines.size()) == maxLines - 1) {
        // Last available line: combine any word already started on this line with
        // the rest of the text as written, then let truncate fit it with an ellipsis.
        std::string lastContent = currentLine;
        if (!lastContent.empty() && firstPiece) lastContent += ' ';
        lastContent.append(text + wordStart + from);
        lines.push_back(truncate(lastContent));
        return lines;
      }

      const size_t to = b < breaks.size() ? breaks[b] : wordText.size();
      const std::string piece = wordText.substr(from, to - from);
      from = to;
      std::string testLine = currentLine.empty() ? piece : currentLine + (firstPiece ? " " : "") + piece;

      if (width(testLine) <= maxWidth) {
        currentLine = testLine;
      } else {
        if (!currentLine.empty()) {
          lines.push_back(currentLine);
          // If the carried-over piece itself exceeds maxWidth, truncate it and
          // push it as a complete line immediately — storing it in currentLine
          // would allow a subsequent short word to be appended after the ellipsis.
          if (width(piece) > maxWidth) {
            lines.push_back(truncate(piece));
            currentLine.clear();
            if (static_cast<int>(lines.size()) >= maxLines) return lines;
          } else {
            currentLine = piece;
          }
        } else {
          // Single word wider than maxWidth: truncate and stop to avoid complicated
          // splitting rules (different between languages). Results in an aesthetically
          // pleasing end.
          lines.push_back(truncate(piece));
          return lines;
        }
      }
    }
  }

  if (!currentLine.empty() && static_cast<int>(lines.size()) < maxLines) {
    lines.push_back(currentLine);
  }

  return lines;
}

}  // namespace textWrap
