#pragma once
#include <cstdint>

class TwoWire {
 public:
  void begin(int = 0, int = 0, int = 0) {}
  void end() {}
  void setTimeOut(int) {}
  void beginTransmission(uint8_t) {}
  void write(uint8_t) {}
  int endTransmission(bool = true) { return 1; }
  int requestFrom(uint8_t, uint8_t, uint8_t = true) { return 0; }
  int available() { return 0; }
  uint8_t read() { return 0; }
};
extern TwoWire Wire;
