#pragma once

#include <cstddef>
#include <cstdint>

class Print {
 public:
  virtual ~Print() = default;
  virtual std::size_t write(std::uint8_t c) = 0;
  virtual std::size_t write(const std::uint8_t* buffer, std::size_t size) {
    for (std::size_t i = 0; i < size; i++) write(buffer[i]);
    return size;
  }
};
