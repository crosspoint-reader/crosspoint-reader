#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>

class FsFile {
 public:
  FsFile() = default;
  FsFile(const FsFile&) = delete;
  FsFile& operator=(const FsFile&) = delete;
  FsFile(FsFile&&) = default;
  FsFile& operator=(FsFile&&) = default;

  explicit operator bool() const { return stream_.is_open(); }

  size_t available() {
    if (!stream_.is_open()) {
      return 0;
    }
    const auto current = stream_.tellg();
    stream_.seekg(0, std::ios::end);
    const auto end = stream_.tellg();
    stream_.seekg(current);
    if (current < 0 || end < 0 || end < current) {
      return 0;
    }
    return static_cast<size_t>(end - current);
  }

  size_t read(void* buffer, const size_t size) {
    if (!stream_.is_open()) {
      return 0;
    }
    stream_.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
    return static_cast<size_t>(stream_.gcount());
  }

  size_t write(const uint8_t value) { return write(&value, 1); }

  size_t write(const void* buffer, const size_t size) {
    if (!stream_.is_open()) {
      return 0;
    }
    stream_.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size));
    return stream_ ? size : 0;
  }

  void close() {
    if (stream_.is_open()) {
      stream_.close();
    }
  }

 private:
  friend struct TestStorage;
  std::fstream stream_;
};

struct TestStorage {
  bool exists(const char* path) const;
  bool remove(const char* path) const;
  bool openFileForRead(const char*, const std::string& path, FsFile& file) const;
  bool openFileForWrite(const char*, const std::string& path, FsFile& file) const;
};

inline TestStorage Storage;
