#pragma once
#include <cstdint>

inline unsigned long millis() { return 0; }

class HardwareSerialStub {
 public:
  template <typename... Args>
  void printf(const char*, Args...) {}
};

static HardwareSerialStub Serial;
