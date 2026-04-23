#pragma once

#include <cstddef>
#include <string>

class WriterCursor {
 public:
  static size_t clamp(const std::string& text, size_t cursor);
  static size_t moveLeft(const std::string& text, size_t cursor);
  static size_t moveRight(const std::string& text, size_t cursor);
};
