#pragma once

#include <Utf8.h>

#include <cstdint>
#include <string>
#include <vector>

// Greedy word-wrap for GfxRenderer::wrappedText(), header-only so the pure
// line-breaking is unit-testable with a mock width (no GfxRenderer/display/font).
//   measure(s)         -> width of s, in the same units as maxWidth
//   truncateToWidth(s) -> s ellipsized to maxWidth (final line only)
// Breaks on ASCII spaces; a token wider than maxWidth is hard-broken at UTF-8
// codepoint boundaries rather than dropped (#2949). This is codepoint-level, not
// full Unicode line-breaking/grapheme segmentation (future work).
namespace textwrap {

template <typename Measure, typename TruncateToWidth>
std::vector<std::string> wrapLines(const char* text, const int maxWidth, const int maxLines, Measure measure,
                                   TruncateToWidth truncateToWidth) {
  std::vector<std::string> lines;
  if (!text || maxWidth <= 0 || maxLines <= 0) return lines;

  std::string remaining = text;
  std::string currentLine;

  // Longest prefix of s (>= 1 codepoint) that fits maxWidth, as a byte length on
  // a UTF-8 boundary. Takes one codepoint even if it overflows, to make progress.
  const auto prefixWithinWidth = [&](const std::string& s) -> size_t {
    const char* base = s.c_str();
    const uint8_t* p = reinterpret_cast<const uint8_t*>(base);
    size_t lastFit = 0;
    while (*p) {
      const uint8_t* q = p;
      utf8NextCodepoint(&q);
      const size_t cand = static_cast<size_t>(reinterpret_cast<const char*>(q) - base);
      if (measure(s.substr(0, cand)) <= maxWidth) {
        lastFit = cand;
        p = q;
      } else {
        break;
      }
    }
    if (lastFit == 0) {
      const uint8_t* q = reinterpret_cast<const uint8_t*>(base);
      utf8NextCodepoint(&q);
      lastFit = static_cast<size_t>(reinterpret_cast<const char*>(q) - base);
    }
    return lastFit;
  };

  while (!remaining.empty()) {
    if (static_cast<int>(lines.size()) == maxLines - 1) {
      // Last available line: fold in the rest and ellipsize.
      lines.push_back(truncateToWidth(currentLine.empty() ? remaining : currentLine + " " + remaining));
      return lines;
    }

    size_t spacePos = remaining.find(' ');
    std::string word;
    if (spacePos == std::string::npos) {
      word = remaining;
      remaining.clear();
    } else {
      word = remaining.substr(0, spacePos);
      remaining.erase(0, spacePos + 1);
    }

    std::string testLine = currentLine.empty() ? word : currentLine + " " + word;

    if (measure(testLine) <= maxWidth) {
      currentLine = testLine;
    } else if (!currentLine.empty()) {
      // Doesn't fit: flush and re-queue `word` so it's handled fresh next line.
      lines.push_back(currentLine);
      currentLine.clear();
      if (static_cast<int>(lines.size()) >= maxLines) return lines;
      remaining = remaining.empty() ? word : word + " " + remaining;
    } else {
      // currentLine empty, so testLine == word and word alone exceeds maxWidth:
      // hard-break at a codepoint boundary and re-queue the rest (#2949).
      const size_t take = prefixWithinWidth(word);
      lines.push_back(word.substr(0, take));
      const std::string rest = word.substr(take);
      remaining = remaining.empty() ? rest : rest + " " + remaining;
    }
  }

  if (!currentLine.empty() && static_cast<int>(lines.size()) < maxLines) {
    lines.push_back(currentLine);
  }

  return lines;
}

}  // namespace textwrap
