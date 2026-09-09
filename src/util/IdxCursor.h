#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Forward-reading window over a sorted StarDict index file. Every HalFile call
// takes the storage mutex, so the linear scans in Dictionary read a block at a
// time rather than a byte at a time: a lookup walks up to SAMPLE_INTERVAL
// entries, which costs thousands of mutex round-trips when read one byte per
// call. The window is deliberately small — locateSynonym() tail-calls
// locateByOrdinal(), so two of these can be live on the stack at once.
//
// Templated on the file type only so the host-side test can substitute a
// memory-backed stub; the firmware instantiates it for HalFile alone. `File`
// needs one member: `int read(void* dest, size_t count)`, returning the byte
// count, or <= 0 at EOF / on error.
template <typename File>
class IdxCursor {
 public:
  // `file` must already be positioned at `startByte`.
  IdxCursor(File& file, uint32_t startByte) : file(file), logicalPos(startByte) {}

  // Byte offset of the next unread byte, as if the file were read unbuffered.
  uint32_t position() const { return logicalPos; }

  // Copy the NUL-terminated word at the cursor into `out`. Returns its length,
  // or -1 at EOF / on I/O error. A word longer than the buffer is truncated,
  // but consumed in full so the cursor stays entry-aligned.
  int readWord(char* out, size_t outSize) {
    size_t i = 0;
    for (;;) {
      if (pos == len && !fill()) return -1;
      const char ch = static_cast<char>(window[pos++]);
      logicalPos++;
      if (ch == '\0') {
        out[i] = '\0';
        return static_cast<int>(i);
      }
      if (i + 1 < outSize) out[i++] = ch;
    }
  }

  // Read exactly `n` bytes. False at EOF / on I/O error.
  bool readBytes(void* dest, size_t n) {
    auto* d = static_cast<uint8_t*>(dest);
    while (n > 0) {
      if (pos == len && !fill()) return false;
      const size_t take = std::min(n, len - pos);
      memcpy(d, window + pos, take);
      d += take;
      pos += take;
      n -= take;
      logicalPos += take;
    }
    return true;
  }

 private:
  bool fill() {
    const int n = file.read(window, sizeof(window));
    pos = 0;
    len = n > 0 ? static_cast<size_t>(n) : 0;
    return len > 0;
  }

  File& file;
  uint32_t logicalPos;
  size_t pos = 0;
  size_t len = 0;
  uint8_t window[256];
};
