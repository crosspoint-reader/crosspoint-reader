#pragma once

#include <Utf8.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Greedy word-wrap for GfxRenderer::wrappedText(), header-only so the pure
// line-breaking is unit-testable with a mock width (no GfxRenderer/display/font).
//   measure(s)         -> width of s, in the same units as maxWidth
//   truncateToWidth(s) -> s ellipsized to maxWidth (final line only)
// Breaks on ASCII spaces; a token wider than maxWidth is hard-broken at UTF-8
// codepoint boundaries rather than dropped (#2949). This is codepoint-level, not
// full Unicode line-breaking/grapheme segmentation (future work).
// Every emitted line is a contiguous slice of the input, so traversal is
// allocation-free: the only heap use is the returned line strings.
namespace textwrap {

template <typename Measure, typename TruncateToWidth>
std::vector<std::string> wrapLines(const char* text, const int maxWidth, const int maxLines, Measure measure,
                                   TruncateToWidth truncateToWidth) {
  std::vector<std::string> lines;
  if (!text || maxWidth <= 0 || maxLines <= 0) return lines;

  const std::string_view in(text);
  // No line is emptier than one codepoint, so lines can't exceed the byte count;
  // cap the reservation so a huge maxLines on short text can't over-allocate.
  lines.reserve(std::min(static_cast<size_t>(maxLines), in.size()));
  std::string scratch;  // reused for width probes; capacity is retained across calls

  // Width of the byte range in[start, start+len), via the reused scratch buffer.
  const auto measureRange = [&](const size_t start, const size_t len) -> int {
    scratch.assign(in.data() + start, len);
    return measure(scratch);
  };

  // Length of in[start, start+len) with any trailing spaces dropped, so a line
  // that swallowed a separator run does not carry blanks that would offset a
  // centred draw.
  const auto trimTrailingSpaces = [&](const size_t start, size_t len) -> size_t {
    while (len > 0 && in[start + len - 1] == ' ') --len;
    return len;
  };

  // Longest prefix of in[start, start+len) (>= 1 codepoint) that fits maxWidth,
  // as a byte length on a UTF-8 boundary. Takes one codepoint even if it
  // overflows, to make progress. Walks boundaries via the lead byte only, so it
  // never reads past the range even on truncated/invalid input.
  const auto prefixWithinWidth = [&](const size_t start, const size_t len) -> size_t {
    size_t consumed = 0;
    size_t lastFit = 0;
    while (consumed < len) {
      size_t step = static_cast<size_t>(utf8CodepointLen(static_cast<uint8_t>(in[start + consumed])));
      if (step > len - consumed) step = len - consumed;  // clamp to the range end
      const size_t cand = consumed + step;
      if (measureRange(start, cand) <= maxWidth) {
        lastFit = cand;
        consumed = cand;
      } else {
        break;
      }
    }
    if (lastFit == 0) {
      lastFit = static_cast<size_t>(utf8CodepointLen(static_cast<uint8_t>(in[start])));
      if (lastFit > len) lastFit = len;
    }
    return lastFit;
  };

  size_t pos = 0;         // start of the next unprocessed word
  size_t lineStart = 0;   // start of the current line's committed content
  size_t lineLen = 0;     // its byte length
  bool haveLine = false;  // whether the current line has any content

  while (pos < in.size()) {
    // Collapse a run of separators. Without this a zero-length "word" between
    // two spaces (or at the start) would start a line, and flushing it emits an
    // empty or whitespace-only line.
    if (in[pos] == ' ' && !haveLine) {
      ++pos;
      continue;
    }

    if (static_cast<int>(lines.size()) == maxLines - 1) {
      // Last available line: fold in the rest and ellipsize.
      const size_t start = haveLine ? lineStart : pos;
      scratch.assign(in.data() + start, in.size() - start);
      lines.push_back(truncateToWidth(scratch));
      return lines;
    }

    const size_t space = in.find(' ', pos);
    const size_t wordEnd = (space == std::string_view::npos) ? in.size() : space;
    const size_t candStart = haveLine ? lineStart : pos;

    if (measureRange(candStart, wordEnd - candStart) <= maxWidth) {
      lineStart = candStart;
      lineLen = wordEnd - candStart;
      haveLine = true;
      pos = (space == std::string_view::npos) ? in.size() : space + 1;  // consume word (+ space)
    } else if (haveLine) {
      // Doesn't fit: flush and re-examine `word` fresh on the next line.
      lines.emplace_back(in.data() + lineStart, trimTrailingSpaces(lineStart, lineLen));
      haveLine = false;
      lineLen = 0;
    } else {
      // Empty line and the word alone exceeds maxWidth: hard-break at a
      // codepoint boundary and leave the rest for the next line (#2949).
      const size_t take = prefixWithinWidth(pos, wordEnd - pos);
      lines.emplace_back(in.data() + pos, take);
      pos += take;
      // If the break consumed the whole word, step past its trailing space so
      // the next iteration does not see it as an empty word.
      if (pos == wordEnd && space != std::string_view::npos) ++pos;
    }
  }

  if (haveLine && static_cast<int>(lines.size()) < maxLines) {
    lines.emplace_back(in.data() + lineStart, trimTrailingSpaces(lineStart, lineLen));
  }

  return lines;
}

}  // namespace textwrap
