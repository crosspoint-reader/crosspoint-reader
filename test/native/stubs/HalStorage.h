#pragma once
// Stub for native unit tests — all storage ops are no-ops / return safe defaults
#include "Arduino.h"
#include <cstddef>
#include <cstdint>
#include <string>

struct FsFile {
  bool operator!() const { return true; }
  explicit operator bool() const { return false; }
  size_t fileSize() const { return 0; }
  int read() { return -1; }
  size_t read(uint8_t*, size_t) { return 0; }
  size_t read(char*, size_t n) { return 0; }
  size_t write(const uint8_t*, size_t) { return 0; }
  void close() {}
  bool isDirectory() const { return false; }
  bool seekCur(int32_t) { return false; }
};

struct HalStorageClass {
  bool exists(const char*) { return false; }
  bool remove(const char*) { return false; }
  bool rename(const char*, const char*) { return false; }
  bool mkdir(const char*) { return false; }
  bool openFileForRead(const char*, const char*, FsFile&) { return false; }
  bool openFileForWrite(const char*, const char*, FsFile&) { return false; }
  FsFile open(const char*) { return {}; }
  std::string readFile(const char*) { return {}; }
  bool writeFile(const char*, const std::string&) { return false; }
  size_t readFileToBuffer(const char*, char*, size_t, size_t = 0) { return 0; }
};
inline HalStorageClass Storage;
