#include "WriterWrappedLayout.h"

#include <Utf8.h>

#include <algorithm>

namespace {

size_t nextCodepointOffset(const std::string& text, size_t offset) {
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(text.data() + offset);
  utf8NextCodepoint(&ptr);
  return static_cast<size_t>(ptr - reinterpret_cast<const unsigned char*>(text.data()));
}

WriterWrappedLayout::Line makeLine(const std::string& text, size_t startOffset, size_t endOffset) {
  return WriterWrappedLayout::Line{text.substr(startOffset, endOffset - startOffset), startOffset, endOffset};
}

bool isWordSeparator(const char ch) { return ch == ' '; }

size_t nextWordCandidateEnd(const std::string& text, size_t offset, const size_t paragraphEnd) {
  while (offset < paragraphEnd && isWordSeparator(text[offset])) {
    ++offset;
  }

  while (offset < paragraphEnd && !isWordSeparator(text[offset])) {
    offset = nextCodepointOffset(text, offset);
  }

  return offset;
}

size_t hardWrapWord(const std::string& text, const size_t wordStart, const size_t wordEnd, const int maxWidth,
                    const WriterWrappedLayout::MeasureText& measureText) {
  size_t offset = wordStart;
  size_t lastFit = wordStart;

  while (offset < wordEnd) {
    const size_t nextOffset = nextCodepointOffset(text, offset);
    if (nextOffset <= offset) {
      break;
    }

    const std::string candidate = text.substr(wordStart, nextOffset - wordStart);
    if (measureText(candidate) > maxWidth) {
      break;
    }

    lastFit = nextOffset;
    offset = nextOffset;
  }

  if (lastFit > wordStart) {
    return lastFit;
  }

  const size_t nextOffset = nextCodepointOffset(text, wordStart);
  return nextOffset > wordStart ? nextOffset : wordEnd;
}

void appendWrappedParagraph(const std::string& text, size_t paragraphStart, size_t paragraphEnd, int maxWidth,
                            const WriterWrappedLayout::MeasureText& measureText,
                            std::vector<WriterWrappedLayout::Line>& lines) {
  if (paragraphStart == paragraphEnd) {
    lines.push_back({"", paragraphStart, paragraphStart});
    return;
  }

  size_t lineStart = paragraphStart;
  while (lineStart < paragraphEnd) {
    size_t lineEnd = lineStart;

    while (lineEnd < paragraphEnd) {
      const size_t candidateEnd = nextWordCandidateEnd(text, lineEnd, paragraphEnd);
      if (candidateEnd <= lineEnd) {
        break;
      }

      const std::string candidate = text.substr(lineStart, candidateEnd - lineStart);
      if (measureText(candidate) <= maxWidth) {
        lineEnd = candidateEnd;
        continue;
      }

      if (lineEnd == lineStart) {
        lineEnd = hardWrapWord(text, lineStart, candidateEnd, maxWidth, measureText);
      }
      break;
    }

    if (lineEnd >= paragraphEnd) {
      lines.push_back(makeLine(text, lineStart, paragraphEnd));
      break;
    }

    if (lineEnd <= lineStart) {
      break;
    }

    lines.push_back(makeLine(text, lineStart, lineEnd));
    lineStart = lineEnd;
    if (lineStart < paragraphEnd && isWordSeparator(text[lineStart])) {
      ++lineStart;
    }
  }
}

}  // namespace

std::vector<WriterWrappedLayout::Line> WriterWrappedLayout::wrap(const std::string& renderedText, const int maxWidth,
                                                                 const MeasureText& measureText) {
  std::vector<Line> lines;
  const int wrappedWidth = std::max(1, maxWidth);

  size_t paragraphStart = 0;
  while (paragraphStart <= renderedText.size()) {
    const size_t newline = renderedText.find('\n', paragraphStart);
    const size_t paragraphEnd = newline == std::string::npos ? renderedText.size() : newline;

    appendWrappedParagraph(renderedText, paragraphStart, paragraphEnd, wrappedWidth, measureText, lines);

    if (newline == std::string::npos) {
      break;
    }
    paragraphStart = newline + 1;
  }

  return lines;
}
