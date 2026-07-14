#pragma once

// Host-test stub for lib/hal/HalStorage.h, whose real implementation wraps
// SdFat behind a FreeRTOS mutex and cannot compile on the host. This stub
// implements the subset of HalFile / HalStorage that CssParser uses, backed
// by stdio on the local filesystem.

#include <cstdint>
#include <cstdio>
#include <string>

class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }

  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool open(const char* path, const char* mode) {
    close();
    fp_ = std::fopen(path, mode);
    return fp_ != nullptr;
  }

  int available() const {
    if (!fp_) return 0;
    const long pos = std::ftell(fp_);
    std::fseek(fp_, 0, SEEK_END);
    const long end = std::ftell(fp_);
    std::fseek(fp_, pos, SEEK_SET);
    return static_cast<int>(end - pos);
  }

  int read(void* buf, size_t count) {
    if (!fp_) return -1;
    return static_cast<int>(std::fread(buf, 1, count, fp_));
  }

  size_t write(const void* buf, size_t count) {
    if (!fp_) return 0;
    return std::fwrite(buf, 1, count, fp_);
  }

  size_t write(uint8_t b) { return write(&b, 1); }

  bool close() {
    if (!fp_) return false;
    std::fclose(fp_);
    fp_ = nullptr;
    return true;
  }

  operator bool() const { return fp_ != nullptr; }

 private:
  std::FILE* fp_ = nullptr;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool exists(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
  }

  bool remove(const char* path) { return std::remove(path) == 0; }

  bool openFileForRead(const char*, const char* path, HalFile& file) { return file.open(path, "rb"); }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) { return file.open(path.c_str(), "rb"); }
  bool openFileForWrite(const char*, const char* path, HalFile& file) { return file.open(path, "wb"); }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) { return file.open(path.c_str(), "wb"); }
};

#define Storage HalStorage::getInstance()
