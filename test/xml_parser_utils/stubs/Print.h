#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Host-test stand-in for Arduino's Print base class. Only the write() surface
// used by ContentOpfParser and HalFile is provided.

class Print {
 public:
  virtual ~Print() = default;

  virtual size_t write(uint8_t) = 0;

  virtual size_t write(const uint8_t* buffer, size_t size) {
    size_t n = 0;
    while (size--) {
      n += write(*buffer++);
    }
    return n;
  }

  size_t write(const char* s) { return s ? write(reinterpret_cast<const uint8_t*>(s), std::strlen(s)) : 0; }
};

#include "WString.h"
