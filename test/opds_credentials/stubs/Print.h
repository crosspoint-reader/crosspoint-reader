#pragma once
#include <cstddef>
#include <cstdint>
class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t* data, size_t length) {
    size_t count = 0;
    while (count < length && write(data[count])) ++count;
    return count;
  }
  virtual void flush() {}
};
