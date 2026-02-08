#pragma once
#include <cstdint>
#include <cstring>

class FsFile {
 public:
  FsFile() = default;
  explicit operator bool() const { return false; }
  int available() { return 0; }
  uint64_t size() { return 0; }
  int read(void*, size_t) { return 0; }
  int read(uint8_t* buf, size_t len) { return read(static_cast<void*>(buf), len); }
  size_t write(uint8_t) { return 1; }
  size_t write(const uint8_t*, size_t len) { return len; }
};
