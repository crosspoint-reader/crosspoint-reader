#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

class WriterWrappedLayout {
 public:
  using MeasureText = std::function<int(const std::string& text)>;

  struct Line {
    std::string text;
    size_t startOffset;
    size_t endOffset;
  };

  static std::vector<Line> wrap(const std::string& renderedText, int maxWidth, const MeasureText& measureText);
};
