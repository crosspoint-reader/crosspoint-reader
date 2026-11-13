#pragma once

#include <cstddef>
#include <cstdint>

// Keeps each serial reply (SerialTestApi.cpp) on a line of its own when a write gives up part way,
// as when the host stops reading. Kept free of the device so it can be tested on the host.
class LineFraming {
 public:
  // Output was given up part way (a reply or a frame): the line it was on is still open.
  void markOpen() { open = true; }

  // Writes one reply line (ending in '\n') through `write(data, len)`, which returns false when it
  // gave up. An open line is ended first; if that fails, the reply is not sent at all, as it would
  // join the cut line. False when the reply was not sent whole.
  template <typename Write>
  bool sendLine(Write&& write, const uint8_t* data, const size_t len) {
    static constexpr uint8_t LINE_END = '\n';
    if (open) {
      if (!write(&LINE_END, 1)) return false;
      open = false;
    }
    if (!write(data, len)) {
      open = true;
      return false;
    }
    return true;
  }

 private:
  bool open = false;
};
