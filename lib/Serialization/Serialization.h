#pragma once
#include <HalStorage.h>
#include <Logging.h>

#include <iostream>

namespace serialization {
template <typename T>
void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void writePod(HalFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
void readPod(HalFile& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

inline void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

inline void writeString(HalFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

// Every real writeString() call in this codebase serializes a short field
// (a file path, a title, a ruby-text annotation, ...) -- never raw chapter
// content, which flows through a separate path. A length this large can only
// come from a corrupt/truncated cache file, not a legitimate string; resizing
// to it blindly risks a multi-hundred-KB allocation that fails and, since
// this build has -fno-exceptions, aborts the whole device instead of
// throwing (see CLAUDE.md's "new is not nothrow on ESP32" -- the same
// failure mode applies transitively through std::string/std::vector's
// default allocator, not just a bare `new`).
constexpr uint32_t MAX_SERIALIZED_STRING_LEN = 65535;

// Returns false if len was rejected as corrupt. Callers must treat this as a
// hard stop, not just a bad value for this one field: the length prefix has
// already been consumed but the (untrustworthy) payload bytes have not, so
// the stream is no longer positioned at a field boundary. Any further read
// from this stream/file would parse payload bytes -- or bytes belonging to
// the next field -- as if they were the next field's own data. The caller
// must abort the whole record (matching how e.g. TextBlock::deserialize()
// already returns nullptr on its own corruption checks) rather than pressing
// on.
[[nodiscard]] inline bool readString(std::istream& is, std::string& s) {
  uint32_t len;
  readPod(is, len);
  if (len > MAX_SERIALIZED_STRING_LEN) {
    LOG_ERR("SER", "readString: length %u exceeds max, treating as corrupt", len);
    s.clear();
    return false;
  }
  s.resize(len);
  is.read(&s[0], len);
  return true;
}

[[nodiscard]] inline bool readString(HalFile& file, std::string& s) {
  uint32_t len;
  readPod(file, len);
  if (len > MAX_SERIALIZED_STRING_LEN) {
    LOG_ERR("SER", "readString: length %u exceeds max, treating as corrupt", len);
    s.clear();
    return false;
  }
  s.resize(len);
  file.read(&s[0], len);
  return true;
}
}  // namespace serialization
