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

void appendWrappedParagraph(const std::string& text, size_t paragraphStart, size_t paragraphEnd, int maxWidth,
                            const WriterWrappedLayout::MeasureText& measureText,
                            std::vector<WriterWrappedLayout::Line>& lines) {
  if (paragraphStart == paragraphEnd) {
    lines.push_back({"", paragraphStart, paragraphStart});
    return;
  }

  size_t lineStart = paragraphStart;
  while (lineStart < paragraphEnd) {
    size_t offset = lineStart;
    size_t lastSpace = std::string::npos;
    size_t lastFit = lineStart;

    while (offset < paragraphEnd) {
      const size_t nextOffset = nextCodepointOffset(text, offset);
      if (nextOffset <= offset) {
        break;
      }

      const std::string candidate = text.substr(lineStart, nextOffset - lineStart);
      if (measureText(candidate) > maxWidth) {
        break;
      }

      if (text[offset] == ' ') {
        lastSpace = offset;
      }

      lastFit = nextOffset;
      offset = nextOffset;
    }

    if (offset >= paragraphEnd) {
      lines.push_back(makeLine(text, lineStart, paragraphEnd));
      break;
    }

    if (lastSpace != std::string::npos && lastSpace > lineStart) {
      lines.push_back(makeLine(text, lineStart, lastSpace));
      lineStart = lastSpace + 1;
      continue;
    }

    if (lastFit > lineStart) {
      lines.push_back(makeLine(text, lineStart, lastFit));
      lineStart = lastFit;
      continue;
    }

    const size_t nextOffset = nextCodepointOffset(text, lineStart);
    if (nextOffset <= lineStart) {
      break;
    }
    lines.push_back(makeLine(text, lineStart, nextOffset));
    lineStart = nextOffset;
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
