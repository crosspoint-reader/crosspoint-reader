#pragma once

// Host stand-in for the Arduino <Print.h> base class. TocNavParser derives from
// Print and only ever uses its two write() overloads, so the host test provides
// a minimal abstract base with the same interface.
#include <cstddef>
#include <cstdint>

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t* buffer, size_t size) = 0;
};
