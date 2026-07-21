#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

class Print {
public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t *buffer, size_t size) {
    size_t n = 0;
    while (size--) {
      n += write(*buffer++);
    }
    return n;
  }
  virtual void flush() {}

  // Add missing overloads from Print
  size_t print(const char *s) { return write((const uint8_t *)s, strlen(s)); }
  size_t println(const char *s) {
    size_t n = print(s);
    n += print("\r\n");
    return n;
  }
  size_t println(int n) { return println("1"); }
};
