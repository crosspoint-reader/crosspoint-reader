#pragma once
#include <HalStorage.h>
#include <Logging.h>

#include <iostream>

namespace serialization {

// Upper bound on any length-prefixed string in a cache file. Every string these
// helpers carry is a book metadata field, an EPUB href, an anchor id or a ruby
// annotation -- hundreds of bytes at the very most. The bound exists because the
// length is read from the file before it is trusted: a truncated read leaves it
// unset and a desynced cursor makes it arbitrary, and `resize()` on a bogus
// length is a throwing allocation, which under -fno-exceptions aborts the
// firmware (field crash: bad_alloc -> terminate -> abort on the page-load path).
// Same rationale as the `wc > 10000` guard in TextBlock::deserialize.
inline constexpr uint32_t MAX_STRING_LEN = 4096;

template <typename T>
void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void writePod(HalFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

// Return false when the source could not supply a whole T. Callers that do not
// care may ignore the result, but anything that then uses `value` as a size or
// an offset must check it.
template <typename T>
bool readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
  return is.gcount() == static_cast<std::streamsize>(sizeof(T));
}

template <typename T>
bool readPod(HalFile& file, T& value) {
  return file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T)) == static_cast<int>(sizeof(T));
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

inline bool readString(std::istream& is, std::string& s) {
  uint32_t len = 0;
  if (!readPod(is, len)) {
    s.clear();
    return false;
  }
  if (len > MAX_STRING_LEN) {
    LOG_ERR("SER", "String length %u exceeds maximum %u", len, MAX_STRING_LEN);
    s.clear();
    return false;
  }
  s.resize(len);
  if (len == 0) {
    return true;
  }
  is.read(&s[0], len);
  return is.gcount() == static_cast<std::streamsize>(len);
}

inline bool readString(HalFile& file, std::string& s) {
  uint32_t len = 0;
  if (!readPod(file, len)) {
    s.clear();
    return false;
  }
  if (len > MAX_STRING_LEN) {
    LOG_ERR("SER", "String length %u exceeds maximum %u", len, MAX_STRING_LEN);
    s.clear();
    return false;
  }
  s.resize(len);
  if (len == 0) {
    return true;
  }
  return file.read(&s[0], len) == static_cast<int>(len);
}
}  // namespace serialization
