#pragma once

#include <cstddef>
#include <string>
#include <utility>

// Puts the serial command lines (CMD:...) together from whatever bytes have arrived, across as many
// loop passes as the line takes, so that reading them never waits for the rest of a line.
//
// The loop used to read them with readStringUntil('\n'), which waits up to the Stream timeout, 1 s,
// for the newline: a stray byte (a monitor attaching to the port) or a key typed into a terminal
// that sends each keystroke stopped the whole loop for a second. Based on crosspoint-reader PR #3438
// (Phillip Suh, @suhsoohong), which found the stall; its 20 ms timeout would cut a line typed a key
// at a time into pieces, so the line is assembled here instead.
//
// A line ends at '\n' or '\r', so a terminal that sends CR, LF or CRLF for Enter all work; the empty
// line between the two of a CRLF is not reported. A line longer than MAX_LINE_BYTES is noise, not a
// command: it is dropped whole, up to its end.
class SerialLineBuffer {
 public:
  // CMD:ACT:READER:<path> carries a full path; a long file name alone is up to 765 bytes of UTF-8.
  static constexpr size_t MAX_LINE_BYTES = 1024;

  // Takes one received byte. True when it completed a line, which take() then hands over.
  bool feed(const char c) {
    if (c == '\n' || c == '\r') {
      const bool complete = !overflowed_ && !pending_.empty();
      overflowed_ = false;
      if (!complete) pending_.clear();
      return complete;
    }
    if (overflowed_) return false;
    if (pending_.size() >= MAX_LINE_BYTES) {
      overflowed_ = true;
      // Not just clear(): a kilobyte of noise must not stay allocated until the next long line.
      std::string().swap(pending_);
      return false;
    }
    pending_ += c;
    return false;
  }

  // The line feed() completed, without its line end; the buffer starts over empty.
  std::string take() { return std::exchange(pending_, std::string()); }

  // Drops a part line, e.g. once the port is handed to a binary protocol that the rest belongs to.
  void reset() {
    std::string().swap(pending_);
    overflowed_ = false;
  }

 private:
  std::string pending_;
  bool overflowed_ = false;
};
