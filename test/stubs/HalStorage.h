#pragma once
// Shared host-test stub for the project HAL (lib/hal/HalStorage.h), which is
// device-only (pulls in Arduino Print, SdFat, FreeRTOS). Backs HalFile with a
// real C FILE* so file-reading code (DictHtmlRenderer, Dictionary) can be
// exercised against actual fixture files on the dev host. Only the subset those
// modules use is implemented.

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>

#define O_RDONLY 0x01
#define O_WRITE 0x02
#define O_CREAT 0x04
#define O_AT_END 0x08
#define O_TRUNC 0x10

class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  HalFile(HalFile&& other) noexcept {
    fp_ = other.fp_;
    other.fp_ = nullptr;
  }
  HalFile& operator=(HalFile&& other) noexcept {
    if (this != &other) {
      close();
      fp_ = other.fp_;
      other.fp_ = nullptr;
    }
    return *this;
  }

  bool openMode(const char* path, const char* mode) {
    close();
    fp_ = std::fopen(path, mode);
    return fp_ != nullptr;
  }

  bool openForRead(const char* path) { return openMode(path, "rb"); }

  bool openForWrite(const char* path) { return openMode(path, "wb"); }

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

  operator bool() const { return fp_ != nullptr; }

 private:
  std::FILE* fp_ = nullptr;
};

class HalStorage {
 private:
  std::string resolvePath(const std::string& path) {
    if (path.rfind("/.crosspoint", 0) == 0) {
      std::string dir = "/tmp/.crosspoint";
      std::error_code ec;
      std::filesystem::create_directories(dir);
      return "/tmp" + path;
    }
    return path;
  }

 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
  bool openFileForRead(const char* /*module*/, const char* path, HalFile& file) {
    return file.openForRead(resolvePath(path).c_str());
  }
  bool openFileForRead(const char* /*module*/, const std::string& path, HalFile& file) {
    return file.openForRead(resolvePath(path).c_str());
  }
  bool exists(const char* path) {
    std::FILE* f = std::fopen(resolvePath(path).c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
  }
  bool openFileForWrite(const char* /*module*/, const char* path, HalFile& file) {
    return file.openForWrite(resolvePath(path).c_str());
  }

  HalFile open(const char* path, int oflag = O_RDONLY) {
    HalFile file;
    const char* mode = "rb";
    if (oflag & O_AT_END) {
      mode = "ab";
    } else if (oflag & O_TRUNC) {
      mode = "wb";
    } else if (oflag & O_WRITE) {
      mode = "wb";
    }
    file.openMode(resolvePath(path).c_str(), mode);
    return file;
  }
};

#define Storage HalStorage::getInstance()
