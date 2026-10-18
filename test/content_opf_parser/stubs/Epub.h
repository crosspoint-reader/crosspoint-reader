#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

inline std::string opfTestBytes;
inline bool opfFailReads = false;

class HalFile {
 public:
  explicit operator bool() const { return open_; }
  void close() {
    EXPECT_TRUE(initialized_) << "Closing a default HalFile asserts on device";
    open_ = false;
  }
  bool seek(size_t position) {
    position_ = position;
    return true;
  }
  size_t position() const { return position_; }
  int available() const { return static_cast<int>(opfTestBytes.size() - position_); }
  size_t read(void* bytes, size_t size) {
    if (opfFailReads) return 0;
    size = std::min(size, opfTestBytes.size() - position_);
    std::memcpy(bytes, opfTestBytes.data() + position_, size);
    position_ += size;
    return size;
  }
  size_t write(const void* bytes, size_t size) {
    opfTestBytes.replace(position_, size, static_cast<const char*>(bytes), size);
    position_ += size;
    return size;
  }
  void markOpen() {
    position_ = 0;
    initialized_ = true;
    open_ = true;
  }

 private:
  bool open_ = false;
  bool initialized_ = false;
  size_t position_ = 0;
};

struct TestStorage {
  int writeOpens = 0;
  int readOpens = 0;

  bool openFileForWrite(const char*, const std::string&, HalFile& file) {
    writeOpens++;
    opfTestBytes.clear();
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
