#pragma once

#include <cstddef>
#include <string_view>

// Assembles newline-terminated lines from bytes that arrive in any split, in a
// fixed buffer. A line longer than the buffer keeps its first N bytes and is
// flagged, so the caller can still name the command it refuses. Pure, so it is
// host-tested; SerialConsole feeds it from the log serial.
template <size_t N>
class SerialLineBuffer {
 public:
  // Takes one byte. True when it ended a line; line() and overflowed() then
  // describe that line until the next call.
  bool feed(const char byte) {
    if (done_) {
      length_ = 0;
      overflowed_ = false;
      done_ = false;
    }
    if (byte == '\n') {
      done_ = true;
      return true;
    }
    if (length_ < N) {
      buffer_[length_++] = byte;
    } else {
      overflowed_ = true;
    }
    return false;
  }

  std::string_view line() const { return {buffer_, length_}; }
  bool overflowed() const { return overflowed_; }

 private:
  char buffer_[N] = {};
  size_t length_ = 0;
  bool overflowed_ = false;
  bool done_ = false;
};
