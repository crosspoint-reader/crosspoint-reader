#pragma once
#include <cstddef>
#include <cstdint>
inline void delay(unsigned) {}
inline unsigned long testMillis = 0;
inline unsigned long millis() { return testMillis; }
struct SilentSerial {
  operator bool() const { return false; }
  template <class... T>
  void printf(const char*, T...) {}
};
inline SilentSerial Serial;

struct FakeEsp {
  unsigned getFreeHeap() const { return 100000; }
  unsigned getMaxAllocHeap() const { return 100000; }
};
inline FakeEsp ESP;
