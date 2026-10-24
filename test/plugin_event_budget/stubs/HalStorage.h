#pragma once
#include <WString.h>
#include <fcntl.h>

#include <cstdint>
#include <string>
#include <unordered_map>
struct HalFile {
  std::string* data = nullptr;
  bool isOpen() const { return data != nullptr; }
  explicit operator bool() const { return isOpen(); }
  size_t write(const char* p, size_t n) {
    if (!data) return 0;
    data->append(p, n);
    return n;
  }
  size_t write(const uint8_t* p, size_t n) { return write(reinterpret_cast<const char*>(p), n); }
  void flush() {}
  void close() { data = nullptr; }
  uint64_t fileSize64() const { return data ? data->size() : 0; }
  size_t fileSize() const { return static_cast<size_t>(fileSize64()); }
  void truncate(uint64_t n) {
    if (data) data->resize(n);
  }
};
struct StorageStub {
  std::unordered_map<std::string, std::string> files;
  bool readFileToString(const char*, const std::string& path, size_t cap, std::string& out) {
    const auto it = files.find(path);
    if (it == files.end() || it->second.size() > cap) return false;
    out = it->second;
    return true;
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& f) {
    files[path].clear();
    f.data = &files[path];
    return true;
  }
  bool openFileForWrite(const char* t, const char* p, HalFile& f) { return openFileForWrite(t, std::string(p), f); }
  HalFile open(const char* path, int) { return HalFile{&files[path]}; }
  bool remove(const char* path) { return files.erase(path) != 0; }
  bool exists(const char* path) const { return files.find(path) != files.end(); }
  bool rename(const char* from, const char* to) {
    auto it = files.find(from);
    if (it == files.end() || exists(to)) return false;
    std::string value = std::move(it->second);
    files.erase(it);
    files[to] = std::move(value);
    return true;
  }
  bool writeFile(const char* path, const String& value) {
    files[path] = value.value;
    return true;
  }
};
inline StorageStub Storage;
