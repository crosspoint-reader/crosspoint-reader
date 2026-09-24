#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }

  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool open(const char* path, const char* mode) {
    close();
    file_ = std::fopen(path, mode);
    return file_ != nullptr;
  }

  int available() const {
    if (!file_) return 0;
    const long position = std::ftell(file_);
    if (position < 0 || std::fseek(file_, 0, SEEK_END) != 0) return 0;
    const long end = std::ftell(file_);
    std::fseek(file_, position, SEEK_SET);
    return end >= position ? static_cast<int>(end - position) : 0;
  }

  int read(void* buffer, size_t count) {
    if (!file_) return -1;
    return static_cast<int>(std::fread(buffer, 1, count, file_));
  }

  size_t write(const void* buffer, size_t count) { return file_ ? std::fwrite(buffer, 1, count, file_) : 0; }

  size_t write(uint8_t byte) { return write(&byte, 1); }

  bool seekCur(size_t count) { return file_ && std::fseek(file_, static_cast<long>(count), SEEK_CUR) == 0; }

  bool close() {
    if (!file_) return false;
    const bool ok = std::fclose(file_) == 0;
    file_ = nullptr;
    return ok;
  }

  explicit operator bool() const { return file_ != nullptr; }

 private:
  std::FILE* file_ = nullptr;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool exists(const char* path) const {
    std::FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    std::fclose(file);
    return true;
  }

  bool remove(const char* path) { return std::remove(path) == 0; }
  bool rename(const char* from, const char* to) {
    if (from == failedRenameFrom_ && to == failedRenameTo_) {
      clearFailures();
      return false;
    }
    return std::rename(from, to) == 0;
  }

  void failNextRename(std::string from, std::string to) {
    failedRenameFrom_ = std::move(from);
    failedRenameTo_ = std::move(to);
  }

  void clearFailures() {
    failedRenameFrom_.clear();
    failedRenameTo_.clear();
  }

  bool openFileForRead(const char*, const char* path, HalFile& file) { return file.open(path, "rb"); }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) { return file.open(path.c_str(), "rb"); }
  bool openFileForWrite(const char*, const char* path, HalFile& file) { return file.open(path, "wb"); }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) { return file.open(path.c_str(), "wb"); }

 private:
  std::string failedRenameFrom_;
  std::string failedRenameTo_;
};

#define Storage HalStorage::getInstance()
