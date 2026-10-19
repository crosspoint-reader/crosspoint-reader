#pragma once
#include <cstdint>
struct UsbStub {
  bool VID(uint16_t) { return true; }
  bool PID(uint16_t) { return true; }
  bool firmwareVersion(uint16_t) { return true; }
  bool productName(const char*) { return true; }
  bool begin() { return true; }
};
inline UsbStub USB;
