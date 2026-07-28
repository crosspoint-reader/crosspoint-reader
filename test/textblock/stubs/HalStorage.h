#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Host-test stub for lib/hal/HalStorage.h. The real HalFile wraps an SdFat
// FsFile behind a FreeRTOS mutex; here it is a plain in-memory byte buffer with
// the same read/write/seek surface, which is all Serialization.h and the block
// serialize/deserialize paths use.
//
// Only the members those paths touch are provided -- if a future test needs
// more of the real API, add it here rather than reaching for the SD stack.
class HalFile {
  std::vector<uint8_t> buf;
  size_t pos = 0;

 public:
  HalFile() = default;

  // Test helpers (not part of the real HalFile API).
  const std::vector<uint8_t>& bytes() const { return buf; }
  void rewind() { pos = 0; }

  size_t size() const { return buf.size(); }
  size_t position() const { return pos; }
  int available() const { return static_cast<int>(buf.size() - pos); }
  explicit operator bool() const { return true; }

  bool seek(const size_t p) {
    if (p > buf.size()) return false;
    pos = p;
    return true;
  }

  int read(void* dst, const size_t count) {
    const size_t n = std::min(count, buf.size() - pos);
    if (n > 0) std::memcpy(dst, buf.data() + pos, n);
    pos += n;
    return static_cast<int>(n);
  }

  size_t write(const void* src, const size_t count) {
    if (pos + count > buf.size()) buf.resize(pos + count);
    std::memcpy(buf.data() + pos, src, count);
    pos += count;
    return count;
  }
};
