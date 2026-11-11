#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// In-memory read-only file with the subset of HalFile that XtcParser uses.
class HalFile {
  friend class HalStorage;

 public:
  uint64_t fileSize64() const { return data ? data->size() : 0; }
  bool seek(uint64_t offset) { return seek64(offset); }
  bool seek64(uint64_t offset) {
    if (!data || offset > data->size()) return false;
    pos = static_cast<size_t>(offset);
    return true;
  }
  size_t read(void* dst, size_t len) {
    if (!data) return 0;
    const size_t remaining = data->size() - pos;
    const size_t count = len < remaining ? len : remaining;
    if (count > 0) std::memcpy(dst, data->data() + pos, count);
    pos += count;
    return count;
  }
  bool close() {
    data = nullptr;
    pos = 0;
    return true;
  }
  bool isOpen() const { return data != nullptr; }

 private:
  const std::vector<uint8_t>* data = nullptr;
  size_t pos = 0;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    if (path != filePath) return false;
    file.data = &fileData;
    file.pos = 0;
    return true;
  }

  void setFile(const std::string& path, std::vector<uint8_t> bytes) {
    filePath = path;
    fileData = std::move(bytes);
  }

 private:
  std::string filePath;
  std::vector<uint8_t> fileData;
};

#define Storage HalStorage::getInstance()
