#pragma once

#include <cstddef>
#include <string>
#include <vector>

class WriterWrappedLayout {
 public:
  struct MeasureText {
    void* context = nullptr;
    int (*fn)(void* context, const std::string& text) = nullptr;

    int operator()(const std::string& text) const { return fn ? fn(context, text) : 0; }
  };

  struct Line {
    size_t startOffset;
    size_t endOffset;
  };

  static std::vector<Line> wrap(const std::string& renderedText, int maxWidth, const MeasureText& measureText);
};
