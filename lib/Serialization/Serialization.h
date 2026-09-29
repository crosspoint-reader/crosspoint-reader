#pragma once
#include <HalStorage.h>

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

inline void readString(std::istream& is, std::string& s) {
  uint32_t len;
  readPod(is, len);
  s.resize(len);
  is.read(&s[0], len);
}

// Returns false on a short read or a length exceeding the remaining file bytes, so a
// corrupt cache can't trigger an oversized (aborting) allocation.
inline bool readString(HalFile& file, std::string& s) {
  uint32_t len;
  if (file.read(&len, sizeof(len)) != static_cast<int>(sizeof(len))) {
    return false;
  }
  const int remaining = file.available();
  if (remaining < 0 || len > static_cast<uint32_t>(remaining)) {
    return false;
  }
  s.resize(len);
  return len == 0 || file.read(&s[0], len) == static_cast<int>(len);
}
}  // namespace serialization
