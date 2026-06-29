#pragma once
// Shared host-test stub for the project HAL (lib/hal/HalStorage.h), which is
// device-only (pulls in Arduino Print, SdFat, FreeRTOS). Backs HalFile with a
// real C FILE* so file-reading code (DictHtmlRenderer, Dictionary) can be
// exercised against actual fixture files on the dev host. Only the subset those
// modules use is implemented.

#include <cstddef>
#include <cstdio>
#include <string>

class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool openForRead(const char* path) {
    close();
    fp_ = std::fopen(path, "rb");
    return fp_ != nullptr;
  }

  bool openForWrite(const char* path) {
    close();
    fp_ = std::fopen(path, "wb");
    return fp_ != nullptr;
  }

  size_t write(const void* buf, size_t n) {
    if (!fp_ || n == 0) return 0;
    return std::fwrite(buf, 1, n, fp_);
  }

  // Single-byte read: returns the next byte (0..255) or -1 on EOF/error.
  int read() {
    if (!fp_) return -1;
    return std::fgetc(fp_);  // EOF == -1
  }

  // Block read: returns the number of bytes read.
  int read(void* buf, int n) {
    if (!fp_ || n <= 0) return 0;
    return static_cast<int>(std::fread(buf, 1, static_cast<size_t>(n), fp_));
  }

  size_t fileSize() {
    if (!fp_) return 0;
    const long cur = std::ftell(fp_);
    std::fseek(fp_, 0, SEEK_END);
    const long end = std::ftell(fp_);
    std::fseek(fp_, cur, SEEK_SET);
    return static_cast<size_t>(end);
  }

  size_t position() const { return fp_ ? static_cast<size_t>(std::ftell(fp_)) : 0; }

  bool available() {
    if (!fp_) return false;
    const int c = std::fgetc(fp_);
    if (c == EOF) return false;
    std::ungetc(c, fp_);
    return true;
  }

  void seekSet(size_t offset) {
    if (fp_) std::fseek(fp_, static_cast<long>(offset), SEEK_SET);
  }

  bool close() {
    if (fp_) {
      std::fclose(fp_);
      fp_ = nullptr;
    }
    return true;
  }

 private:
  std::FILE* fp_ = nullptr;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
  bool openFileForRead(const char* /*module*/, const char* path, HalFile& file) { return file.openForRead(path); }
  bool openFileForRead(const char* /*module*/, const std::string& path, HalFile& file) {
    return file.openForRead(path.c_str());
  }
  bool exists(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
  }
  bool openFileForWrite(const char* /*module*/, const char* path, HalFile& file) { return file.openForWrite(path); }
};

#define Storage HalStorage::getInstance()
