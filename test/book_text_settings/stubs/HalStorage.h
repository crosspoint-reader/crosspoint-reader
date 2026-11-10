#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

class HalFile {
 public:
  std::vector<uint8_t>* bytes = nullptr;
  size_t offset = 0;
  bool shortWrite = false;
  size_t fileSize() const { return bytes ? bytes->size() : 0; }
  int available() const { return static_cast<int>(fileSize() - offset); }
  int read(void* destination, size_t length) {
    if (!bytes) return 0;
    length = std::min(length, bytes->size() - offset);
    std::memcpy(destination, bytes->data() + offset, length);
    offset += length;
    return static_cast<int>(length);
  }
  size_t write(const void* source, size_t length) {
    if (!bytes) return 0;
    if (shortWrite && length) --length;
    const auto* data = static_cast<const uint8_t*>(source);
    bytes->insert(bytes->end(), data, data + length);
    return length;
  }
  void flush() {}
  bool close() {
    bytes = nullptr;
    return true;
  }
};

class HalStorage {
 public:
  std::map<std::string, std::vector<uint8_t>> files;
  bool failWrite = false;
  bool failRemove = false;
  bool shortWrite = false;
  unsigned writes = 0;
  bool exists(const char* path) const { return files.count(path) != 0; }
  bool ensureDirectoryExists(const char*) { return true; }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    if (!exists(path.c_str())) return false;
    file.bytes = &files[path];
    return true;
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    ++writes;
    if (failWrite) return false;
    file.bytes = &files[path];
    file.bytes->clear();
    file.shortWrite = shortWrite;
    return true;
  }
  bool remove(const char* path) {
    if (failRemove) return false;
    return files.erase(path) != 0;
  }
  bool replaceFile(const char* temporary, const char* final) {
    if (!exists(temporary)) return false;
    files[final] = std::move(files[temporary]);
    files.erase(temporary);
    return true;
  }
};
inline HalStorage Storage;
