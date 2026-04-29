#include "WriterWrappedLayout.h"

#include <Utf8.h>

#include <algorithm>

namespace {

size_t nextCodepointOffset(const std::string& text, size_t offset) {
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(text.data() + offset);
  utf8NextCodepoint(&ptr);
  return static_cast<size_t>(ptr - reinterpret_cast<const unsigned char*>(text.data()));
}

WriterWrappedLayout::Line makeLine(size_t startOffset, size_t endOffset) {
  return WriterWrappedLayout::Line{startOffset, endOffset};
}

bool isWordSeparator(const char ch) { return ch == ' '; }

size_t estimateLineCapacity(const std::string& text) {
  size_t lineCapacity = 0;
  bool inWord = false;
  bool paragraphHasContent = false;

  for (const char ch : text) {
    if (ch == '\n') {
      if (!paragraphHasContent) {
        ++lineCapacity;
      }
      inWord = false;
      paragraphHasContent = false;
      continue;
    }

    if (isWordSeparator(ch)) {
      inWord = false;
      continue;
    }

    paragraphHasContent = true;
    if (!inWord) {
      ++lineCapacity;
      inWord = true;
    }
  }

  if (!paragraphHasContent && (text.empty() || text.back() == '\n')) {
    ++lineCapacity;
  }

  return std::max<size_t>(1, lineCapacity);
}

size_t nextWordCandidateEnd(const std::string& text, size_t offset, const size_t paragraphEnd) {
  while (offset < paragraphEnd && isWordSeparator(text[offset])) {
    ++offset;
  }

  while (offset < paragraphEnd && !isWordSeparator(text[offset])) {
    offset = nextCodepointOffset(text, offset);
  }

  return offset;
}

int measureSlice(const std::string& text, const size_t startOffset, const size_t endOffset,
                 const WriterWrappedLayout::MeasureText& measureText, std::string& measureScratch) {
  measureScratch.assign(text, startOffset, endOffset - startOffset);
  return measureText(measureScratch);
}

size_t hardWrapWord(const std::string& text, const size_t wordStart, const size_t wordEnd, const int maxWidth,
                    const WriterWrappedLayout::MeasureText& measureText, std::string& measureScratch) {
  size_t offset = wordStart;
  size_t lastFit = wordStart;

  while (offset < wordEnd) {
    const size_t nextOffset = nextCodepointOffset(text, offset);
    if (nextOffset <= offset) {
      break;
    }

    if (measureSlice(text, wordStart, nextOffset, measureText, measureScratch) > maxWidth) {
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
                            std::vector<WriterWrappedLayout::Line>& lines, std::string& measureScratch) {
  if (paragraphStart == paragraphEnd) {
    lines.push_back(makeLine(paragraphStart, paragraphStart));
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

      if (measureSlice(text, lineStart, candidateEnd, measureText, measureScratch) <= maxWidth) {
        lineEnd = candidateEnd;
        continue;
      }

      if (lineEnd == lineStart) {
        lineEnd = hardWrapWord(text, lineStart, candidateEnd, maxWidth, measureText, measureScratch);
      }
      break;
    }

    if (lineEnd >= paragraphEnd) {
      lines.push_back(makeLine(lineStart, paragraphEnd));
      break;
    }

    if (lineEnd <= lineStart) {
      break;
    }

    lines.push_back(makeLine(lineStart, lineEnd));
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
  lines.reserve(estimateLineCapacity(renderedText));
  const int wrappedWidth = std::max(1, maxWidth);
  std::string measureScratch;

  size_t paragraphStart = 0;
  while (paragraphStart <= renderedText.size()) {
    const size_t newline = renderedText.find('\n', paragraphStart);
    const size_t paragraphEnd = newline == std::string::npos ? renderedText.size() : newline;

    appendWrappedParagraph(renderedText, paragraphStart, paragraphEnd, wrappedWidth, measureText, lines,
                           measureScratch);

    if (newline == std::string::npos) {
      break;
    }
    paragraphStart = newline + 1;
  }

  return lines;
}
