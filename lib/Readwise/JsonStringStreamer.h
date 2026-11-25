#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace readwise {

// Streams the decoded value of one JSON string field (e.g. "html_content")
// out of a JSON response of any size, without buffering the value. Every
// other value is skipped. The first occurrence of the key wins.
//
// Fixed state (~64 bytes); safe to feed in arbitrary chunks.
class JsonStringStreamer {
 public:
  using Sink = std::function<bool(const char* data, size_t len)>;

  JsonStringStreamer(const char* key, Sink sink);

  // Returns false once the sink refused data or the JSON was malformed
  // around the target value.
  bool feed(const char* data, size_t len);

  bool found() const { return found_; }
  bool complete() const { return complete_; }

 private:
  enum class State : uint8_t { Scan, InString, AfterColon, InTarget, Done };

  void scan(char c);
  void inString(char c);
  void inTarget(char c);
  void emitUtf8(uint32_t cp);
  void emit(const char* s, size_t n);
  bool flush();

  static constexpr size_t kKeyMax = 32;
  static constexpr size_t kOutBuf = 512;

  const char* key_;
  size_t keyLen_;
  Sink sink_;
  bool ok_ = true;
  State state_ = State::Scan;

  // Last string seen in Scan state (candidate key).
  char str_[kKeyMax] = {};
  uint8_t strLen_ = 0;
  bool strOverflow_ = false;
  bool lastStringIsKey_ = false;  // the most recent completed string matched key_
  bool escaped_ = false;

  // \uXXXX decoding inside the target.
  uint8_t hexLeft_ = 0;
  uint16_t hex_ = 0;
  uint16_t highSurrogate_ = 0;

  bool found_ = false;
  bool complete_ = false;

  char buf_[kOutBuf];
  size_t bufLen_ = 0;
};

}  // namespace readwise
