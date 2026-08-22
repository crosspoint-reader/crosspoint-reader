#pragma once

#include <cstddef>
#include <cstdint>

class TerminalUsb {
 public:
  void begin();
  int available() const;
  int read() const;
  size_t write(const uint8_t* bytes, size_t size) const;
  size_t write(uint8_t byte) const;
  bool isConnected() const;
};
