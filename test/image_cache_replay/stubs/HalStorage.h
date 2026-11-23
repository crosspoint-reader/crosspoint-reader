#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace FileTest {
inline std::unordered_map<std::string, std::vector<uint8_t>> files;
inline unsigned readOpens = 0;
inline int writesUntilFailure = -1;
}  // namespace FileTest

class HalFile {
 public:
  HalFile() = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  explicit operator bool() const { return isOpen(); }
  int read() {
    uint8_t value;
    return read(&value, 1) == 1 ? value : -1;
  }
  int read(void* output, size_t count) {
    if (!bytes) return 0;
    count = std::min(count, bytes->size() - offset);
    if (count) std::memcpy(output, bytes->data() + offset, count);
    offset += count;
    return count;
  }
  size_t write(const void* input, size_t count) {
    if (!writable || !bytes || FileTest::writesUntilFailure == 0) return 0;
    if (FileTest::writesUntilFailure > 0) --FileTest::writesUntilFailure;
    if (offset + count > bytes->size()) bytes->resize(offset + count);
    if (count) std::memcpy(bytes->data() + offset, input, count);
    offset += count;
    return count;
  }
  size_t position() const { return offset; }
  size_t size() const { return bytes ? bytes->size() : 0; }
  int available() const { return static_cast<int>(size() - offset); }
  bool seek(size_t position) {
    if (!bytes || position > size()) return false;
    offset = position;
    return true;
  }
  bool seekCur(size_t count) { return seek(offset + count); }
  bool close() {
    const bool wasOpen = isOpen();
    bytes = nullptr;
    offset = 0;
    return wasOpen;
  }
  bool isOpen() const { return bytes != nullptr; }

 private:
  friend struct TestStorage;
  std::vector<uint8_t>* bytes = nullptr;
  size_t offset = 0;
  bool writable = false;
};

struct TestStorage {
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    file.close();
    const auto entry = FileTest::files.find(path);
    if (entry == FileTest::files.end()) return false;
    ++FileTest::readOpens;
    file.bytes = &entry->second;
    file.writable = false;
    return true;
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    file.close();
    file.bytes = &FileTest::files[path];
    file.bytes->clear();
    file.writable = true;
    return true;
  }
  bool exists(const char* path) const { return FileTest::files.contains(path); }
  bool remove(const std::string& path) { return FileTest::files.erase(path) != 0; }
};
inline TestStorage Storage;
