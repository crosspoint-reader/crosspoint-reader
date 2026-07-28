#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Host-test stub for lib/hal/HalStorage.h. The real HalFile wraps an SdFat
// FsFile behind a FreeRTOS mutex; here it is a plain in-memory byte buffer.
//
// Serialization.h and TextBlock's serialize/deserialize paths use only read()
// and write(), so only those are provided -- if a future test needs more of the
// real API, add it here rather than reaching for the SD stack.
class HalFile {
  std::vector<uint8_t> buf;
  size_t pos = 0;

 public:
  HalFile() = default;

  // Test helper (not part of the real HalFile API).
  void rewind() { pos = 0; }

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
