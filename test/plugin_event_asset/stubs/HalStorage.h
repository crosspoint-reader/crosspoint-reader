#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

struct HalFile {
  const std::string* data = nullptr;
  size_t pos = 0;
  explicit operator bool() const { return data != nullptr; }
  bool seek(uint64_t offset) {
    if (!data || offset > data->size()) return false;
    pos = static_cast<size_t>(offset);
    return true;
  }
  bool seekCur(int64_t offset) {
    if (offset < 0 && static_cast<uint64_t>(-offset) > pos) return false;
    return seek(pos + offset);
  }
  int read() { return !data || pos >= data->size() ? -1 : static_cast<unsigned char>((*data)[pos++]); }
  size_t read(void* out, size_t count) {
    auto* bytes = static_cast<unsigned char*>(out);
    size_t n = 0;
    while (n < count) {
      const int c = read();
      if (c < 0) break;
      bytes[n++] = static_cast<unsigned char>(c);
    }
    return n;
  }
  uint64_t fileSize64() const { return data ? data->size() : 0; }
};

struct StorageStub {
  std::unordered_map<std::string, std::string> files;
  std::string failRenameFrom, failRenameTo, failRemove;
  bool openFileForRead(const char*, const std::string& path, HalFile& out) {
    const auto it = files.find(path);
    if (it == files.end()) return false;
    out.data = &it->second;
    out.pos = 0;
    return true;
  }
  bool readFileToString(const char*, const std::string& path, size_t cap, std::string& out) {
    const auto it = files.find(path);
    if (it == files.end() || it->second.size() > cap) return false;
    out = it->second;
    return true;
  }
  bool writeFile(const char* path, const std::string& value) {
    files[path] = value;
    return true;
  }
  bool exists(const char* path) const { return files.count(path) != 0; }
  bool rename(const char* from, const char* to) {
    if (failRenameFrom == from || failRenameTo == to || exists(to)) return false;
    const auto it = files.find(from);
    if (it == files.end()) return false;
    files[to] = std::move(it->second);
    files.erase(from);
    return true;
  }
  bool remove(const char* path) {
    if (failRemove == path) return false;
    return files.erase(path) != 0;
  }
};
inline StorageStub Storage;
