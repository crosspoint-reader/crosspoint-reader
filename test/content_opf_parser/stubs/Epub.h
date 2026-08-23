#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class HalFile {
 public:
  explicit operator bool() const { return open_; }
  void close() { open_ = false; }
  bool seek(size_t position) {
    position_ = position;
    return true;
  }
  size_t position() const { return position_; }
  int available() const { return 0; }
  void markOpen() { open_ = true; }

 private:
  bool open_ = false;
  size_t position_ = 0;
};

struct TestStorage {
  int writeOpens = 0;
  int readOpens = 0;

  bool openFileForWrite(const char*, const std::string&, HalFile& file) {
    writeOpens++;
    file.markOpen();
    return true;
  }
  bool openFileForRead(const char*, const std::string&, HalFile& file) {
    readOpens++;
    file.markOpen();
    return true;
  }
  bool exists(const char*) const { return false; }
  bool remove(const char*) { return true; }
};

inline TestStorage Storage;
