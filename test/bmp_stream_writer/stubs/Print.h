#pragma once

#include <cstddef>
#include <cstdint>

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t value) = 0;
  virtual size_t write(const uint8_t* buffer, size_t size) {
    for (size_t i = 0; i < size; i++) write(buffer[i]);
    return size;
  }
};
