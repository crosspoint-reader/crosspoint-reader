#pragma once

#include <cstddef>
#include <string>
#include <vector>

class WriterWrappedLayout {
 public:
  struct Line {
    std::string text;
    size_t startOffset;
    size_t endOffset;
  };

  static std::vector<Line> wrap(const std::string& renderedText, size_t maxColumns);
};
