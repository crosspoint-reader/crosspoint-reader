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

void appendWrappedParagraph(const std::string& text,
                           size_t paragraphStart,
                           size_t paragraphEnd,
                           size_t maxColumns,
                           std::vector<WriterWrappedLayout::Line>& lines) {
  if (paragraphStart == paragraphEnd) {
    lines.push_back({"", paragraphStart, paragraphStart});
    return;
  }

  size_t lineStart = paragraphStart;
  while (lineStart < paragraphEnd) {
    size_t offset = lineStart;
    size_t breakAtSpace = std::string::npos;
    size_t codepoints = 0;

    while (offset < paragraphEnd && codepoints < maxColumns) {
      const size_t nextOffset = nextCodepointOffset(text, offset);
      if (nextOffset <= offset) {
        break;
      }

      if (text[offset] == ' ') {
        breakAtSpace = offset;
      }

      offset = nextOffset;
      ++codepoints;
    }

    if (offset >= paragraphEnd) {
      lines.push_back(makeLine(text, lineStart, paragraphEnd));
      break;
    }

    if (breakAtSpace != std::string::npos && breakAtSpace > lineStart) {
      lines.push_back(makeLine(text, lineStart, breakAtSpace));
      lineStart = breakAtSpace + 1;
      continue;
    }

    lines.push_back(makeLine(text, lineStart, offset));
    lineStart = offset;
  }
}

}  // namespace

std::vector<WriterWrappedLayout::Line> WriterWrappedLayout::wrap(const std::string& renderedText, size_t maxColumns) {
  std::vector<Line> lines;
  maxColumns = std::max<size_t>(1, maxColumns);

  size_t paragraphStart = 0;
  while (paragraphStart <= renderedText.size()) {
    const size_t newline = renderedText.find('\n', paragraphStart);
    const size_t paragraphEnd = newline == std::string::npos ? renderedText.size() : newline;

    appendWrappedParagraph(renderedText, paragraphStart, paragraphEnd, maxColumns, lines);

    if (newline == std::string::npos) {
      break;
    }
    paragraphStart = newline + 1;
  }

  return lines;
}
