#pragma once
#include "Arduino.h"
class Print {
 public:
  virtual size_t write(uint8_t) { return 0; }
  virtual size_t write(const uint8_t*, size_t n) { return n; }
  virtual void flush() {}
  virtual ~Print() = default;
};
