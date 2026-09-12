#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class String : public std::string {
 public:
  using std::string::string;
};

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(const uint8_t*, size_t) = 0;
  virtual size_t write(uint8_t) = 0;
};
