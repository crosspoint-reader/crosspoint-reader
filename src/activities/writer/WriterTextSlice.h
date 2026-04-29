#pragma once

#include <cstddef>
#include <string>

class WriterTextSlice {
 public:
  static std::string slice(const std::string& text, size_t startOffset, size_t endOffset);
};
