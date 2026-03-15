#pragma once

#include <cstdint>

struct TestEspClass {
  uint32_t getFreeHeap() const { return 1024 * 1024; }
};

inline TestEspClass ESP;
