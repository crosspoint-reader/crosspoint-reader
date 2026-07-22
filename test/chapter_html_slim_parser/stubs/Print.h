#pragma once

// Minimal host-test stand-in for Arduino's Print.h. HalFile derives from this
// (matching the real HalStorage.h) so it can be constructed and moved around;
// only write(uint8_t) is ever invoked by the code under test.

#include <cstddef>
#include <cstdint>

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t b) = 0;
  virtual size_t write(const uint8_t* buffer, size_t size) {
    size_t n = 0;
    for (size_t i = 0; i < size; i++) {
      n += write(buffer[i]);
    }
    return n;
  }
};
