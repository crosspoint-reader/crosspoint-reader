#pragma once
#include <HalStorage.h>
#include <Logging.h>

#include <iostream>

namespace serialization {

// A length-prefixed string is bounded by the bytes actually left in the source,
// never by a fixed content cap. The length is read from the file before it can
// be trusted: a truncated read leaves it unset and a desynced cursor makes it
// arbitrary, and `resize()` on a bogus length is a throwing allocation, which
// under -fno-exceptions aborts the firmware (field crash: bad_alloc ->
// terminate -> abort on the page-load path).
//
// A fixed cap would be the wrong bound here. `writeString` is unbounded and so
// is the parser feeding it -- ruby <rt> text accumulates into an unbounded
// std::string in ChapterHtmlSlimParser -- so any cap the reader enforces and
// the writer does not turns a legitimately long string into a cache that
// serializes fine and then fails every reload, rebuilding forever. Bounding by
// remaining bytes rejects exactly the corrupt lengths (which point past EOF)
// while leaving every string the writer can actually produce readable.

template <typename T>
void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void writePod(HalFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

// Return false when the source could not supply a whole T, leaving `value`
// zeroed rather than holding whatever was already on the stack. Callers that do
// not care may ignore the result, but anything that then uses `value` as a size
// or an offset must check it.
template <typename T>
bool readPod(std::istream& is, T& value) {
  value = T{};
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
  return is.gcount() == static_cast<std::streamsize>(sizeof(T));
}

template <typename T>
bool readPod(HalFile& file, T& value) {
  value = T{};
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
  // Bytes left in the stream, restoring the cursor afterwards.
  const auto cur = is.tellg();
  is.seekg(0, std::ios::end);
  const auto endPos = is.tellg();
  is.seekg(cur);
  if (cur < 0 || endPos < cur || len > static_cast<uint64_t>(endPos - cur)) {
    LOG_ERR("SER", "String length %u exceeds %lld bytes remaining", len,
            static_cast<long long>(endPos < cur ? 0 : endPos - cur));
    s.clear();
    return false;
  }
  s.resize(len);
  if (len == 0) {
    return true;
  }
  is.read(&s[0], len);
  if (is.gcount() != static_cast<std::streamsize>(len)) {
    s.clear();
    return false;
  }
  return true;
}

inline bool readString(HalFile& file, std::string& s) {
  uint32_t len = 0;
  if (!readPod(file, len)) {
    s.clear();
    return false;
  }
  const int remaining = file.available();
  if (remaining < 0 || len > static_cast<uint32_t>(remaining)) {
    LOG_ERR("SER", "String length %u exceeds %d bytes remaining", len, remaining);
    s.clear();
    return false;
  }
  s.resize(len);
  if (len == 0) {
    return true;
  }
  if (file.read(&s[0], len) != static_cast<int>(len)) {
    s.clear();
    return false;
  }
  return true;
}
}  // namespace serialization
