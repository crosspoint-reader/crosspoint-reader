#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class FakeHalStorage;

class HalFile {
  friend class FakeHalStorage;

  std::vector<uint8_t>* bytes_ = nullptr;
  size_t position_ = 0;
  size_t* maxWritePerCall_ = nullptr;

  void attach(std::vector<uint8_t>* bytes, size_t* maxWritePerCall) {
    bytes_ = bytes;
    position_ = 0;
    maxWritePerCall_ = maxWritePerCall;
  }

 public:
  int read(void* destination, const size_t count) {
    if (!bytes_ || position_ >= bytes_->size()) return 0;
    const size_t available = bytes_->size() - position_;
    const size_t readCount = std::min(count, available);
    std::memcpy(destination, bytes_->data() + position_, readCount);
    position_ += readCount;
    return static_cast<int>(readCount);
  }

  size_t write(const void* source, const size_t count) {
    if (!bytes_) return 0;
    const size_t writeCount = std::min(count, *maxWritePerCall_);
    if (position_ + writeCount > bytes_->size()) bytes_->resize(position_ + writeCount);
    std::memcpy(bytes_->data() + position_, source, writeCount);
    position_ += writeCount;
    return writeCount;
  }

  void flush() {}
  bool close() {
    bytes_ = nullptr;
    return true;
  }
  size_t size() const { return bytes_ ? bytes_->size() : 0; }
};

class FakeHalStorage {
 public:
  std::unordered_map<std::string, std::vector<uint8_t>> files;
  size_t maxWritePerCall = std::numeric_limits<size_t>::max();

  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    const auto it = files.find(path);
    if (it == files.end()) return false;
    file.attach(&it->second, &maxWritePerCall);
    return true;
  }

  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    auto& bytes = files[path];
    bytes.clear();
    file.attach(&bytes, &maxWritePerCall);
    return true;
  }

  bool remove(const char* path) { return files.erase(path) > 0; }

  bool rename(const char* oldPath, const char* newPath) {
    const auto it = files.find(oldPath);
    if (it == files.end() || files.contains(newPath)) return false;
    files.emplace(newPath, std::move(it->second));
    files.erase(it);
    return true;
  }

  bool exists(const std::string& path) const { return files.contains(path); }

  void reset() {
    files.clear();
    maxWritePerCall = std::numeric_limits<size_t>::max();
  }
};

extern FakeHalStorage Storage;
