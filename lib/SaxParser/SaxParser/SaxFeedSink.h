#pragma once

#include <Print.h>
#include <SaxParser/SaxParser.h>

#include <cstddef>
#include <cstdint>

// Hands inflate output straight to a SaxParser. The XPath mappers used to inflate a chapter to a
// temp file on the SD card and parse that; this is the same parse without the file, and it tells
// the inflate to stop as soon as the parser does (the forward mapper stops at its target).
//
// Stack object, no allocation: a reference, two flags. The parser's own state is the caller's.
class SaxFeedSink final : public Print {
 public:
  explicit SaxFeedSink(SaxParser& parser) : parser_(parser) {}

  size_t write(const uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, const size_t n) override {
    if (stopped_) {
      return n;  // drained, not parsed: the stream loop honours stopFlag() and ends
    }
    if (!parser_.feed(buf, n)) {
      failed_ = !parser_.isStopped();
      stopped_ = true;
    } else if (parser_.isStopped()) {
      stopped_ = true;
    }
    return n;
  }

  // For Epub::readItemContentsToStream: set once the parser stopped or failed.
  const bool* stopFlag() const { return &stopped_; }
  bool failed() const { return failed_; }

 private:
  SaxParser& parser_;
  bool stopped_ = false;
  bool failed_ = false;
};
