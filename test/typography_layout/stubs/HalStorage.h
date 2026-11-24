#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

inline std::vector<uint8_t> sdFontTestFile;
inline size_t sdFontTestReads = 0;

struct TypographyTestEsp {
  size_t largestBlock = 200 * 1024;
  size_t freeHeap = 200 * 1024;
  size_t getFreeHeap() const { return freeHeap; }
  size_t getMaxAllocHeap() const { return largestBlock; }
  void restart() { std::abort(); }
};
inline TypographyTestEsp ESP;
inline uint32_t millis() { return 0; }

class HalFile {
 public:
  HalFile() = default;
  HalFile(HalFile&&) noexcept = default;
  HalFile& operator=(HalFile&&) noexcept = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void openReadBuffer() {
    data_ = &sdFontTestFile;
    position_ = 0;
    opened_ = true;
    writable_ = false;
  }

  void openScratchBuffer(std::vector<uint8_t>& buf, bool writeMode) {
    if (writeMode) buf.clear();
    data_ = &buf;
    position_ = 0;
    opened_ = true;
    writable_ = writeMode;
  }

  bool isOpen() const { return opened_; }
  explicit operator bool() const { return opened_; }
  int available() const {
    return opened_ && data_ && position_ < data_->size() ? static_cast<int>(data_->size() - position_) : 0;
  }
  int read() {
    uint8_t b = 0;
    return read(&b, 1) == 1 ? b : -1;
  }
  bool seekSet(size_t position) {
    position_ = position;
    return opened_ && data_ && position <= data_->size();
  }
  bool seek(size_t position) { return seekSet(position); }
  bool seekCur(size_t offset) { return seekSet(position_ + offset); }
  int read(void* output, size_t count) {
    if (!opened_ || !data_ || position_ > data_->size()) return 0;
    count = std::min(count, data_->size() - position_);
    if (count > 0) {
      std::memcpy(output, data_->data() + position_, count);
      position_ += count;
    }
    sdFontTestReads++;
    return static_cast<int>(count);
  }
  size_t write(const void* input, size_t count) {
    if (!opened_ || !writable_ || !data_) return 0;
    if (position_ + count > data_->size()) {
      data_->resize(position_ + count);
    }
    if (count > 0) {
      std::memcpy(data_->data() + position_, input, count);
      position_ += count;
    }
    return count;
  }
  size_t write(uint8_t b) { return write(&b, 1); }
  bool close() {
    opened_ = false;
    return true;
  }
  size_t position() const { return position_; }
  size_t size() const { return data_ ? data_->size() : 0; }
  size_t fileSize() const { return size(); }

 private:
  std::vector<uint8_t>* data_ = nullptr;
  size_t position_ = 0;
  bool opened_ = false;
  bool writable_ = false;
};

class HalStorage {
 public:
  bool openFileForRead(const char*, const char*, HalFile& file) {
    file.openReadBuffer();
    return true;
  }
};
inline HalStorage Storage;
