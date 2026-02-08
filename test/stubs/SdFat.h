#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

class FsFile {
 public:
  FsFile() = default;
  explicit operator bool() const { return buf_ != nullptr; }

  int available() { return buf_ ? static_cast<int>(buf_->size() - pos_) : 0; }
  uint64_t size() { return buf_ ? buf_->size() : 0; }

  int read(void* dst, size_t len) {
    if (!buf_ || pos_ >= buf_->size()) return 0;
    size_t n = std::min(len, buf_->size() - pos_);
    memcpy(dst, buf_->data() + pos_, n);
    pos_ += n;
    return static_cast<int>(n);
  }
  int read(uint8_t* buf, size_t len) { return read(static_cast<void*>(buf), len); }

  size_t write(uint8_t b) {
    ensureBuf();
    buf_->push_back(b);
    return 1;
  }
  size_t write(const uint8_t* data, size_t len) {
    ensureBuf();
    buf_->insert(buf_->end(), data, data + len);
    return len;
  }

  bool seek(uint64_t pos) { return seekSet(pos); }
  bool seekSet(uint64_t pos) {
    pos_ = static_cast<size_t>(pos);
    return true;
  }
  void close() { pos_ = 0; }

  // Test helpers
  void initBuffer(std::shared_ptr<std::vector<uint8_t>> b) {
    buf_ = b;
    pos_ = 0;
  }
  std::shared_ptr<std::vector<uint8_t>> buffer() const { return buf_; }

 private:
  void ensureBuf() {
    if (!buf_) buf_ = std::make_shared<std::vector<uint8_t>>();
  }
  std::shared_ptr<std::vector<uint8_t>> buf_;
  size_t pos_ = 0;
};
