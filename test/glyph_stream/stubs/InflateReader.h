#pragma once

#include <cstddef>
#include <cstdint>

class InflateReader {
 public:
  bool init(bool = false) { return true; }
  void setSource(const uint8_t*, size_t) {}
  bool read(uint8_t*, size_t) { return false; }
};
