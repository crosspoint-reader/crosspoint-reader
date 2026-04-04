#pragma once
#include <HalStorage.h>

#include <iostream>

namespace serialization {
template <typename T>
static void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
static void writePod(FsFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
static void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
static void readPod(FsFile& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

static void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

static void writeString(FsFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

constexpr uint32_t MAX_STRING_LENGTH = 4096;

static bool readString(std::istream& is, std::string& s) {
  uint32_t len;
  readPod(is, len);
  if (len > MAX_STRING_LENGTH) {
    is.seekg(len, std::ios::cur);  // skip payload to keep stream aligned
    return false;
  }
  s.resize(len);
  is.read(&s[0], len);
  return true;
}

static bool readString(FsFile& file, std::string& s) {
  uint32_t len;
  readPod(file, len);
  if (len > MAX_STRING_LENGTH) {
    if (!file.seekCur(static_cast<int64_t>(len))) {  // skip payload to keep file position aligned
      return false;
    }
    return false;
  }
  s.resize(len);
  file.read(reinterpret_cast<uint8_t*>(&s[0]), len);
  return true;
}
}  // namespace serialization
