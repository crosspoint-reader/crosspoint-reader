#pragma once

// In-memory SD card. Its buffers are test storage, not device heap, so they are
// allocated outside the heap cap.
#include <HeapCap.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct TestFile {
  std::vector<uint8_t> bytes;
};

class HalFile {
 public:
  HalFile() = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  ~HalFile() { close(); }

  explicit operator bool() const { return static_cast<bool>(data); }
  size_t position() const { return pos; }
  size_t size() const { return data ? data->bytes.size() : 0; }
  int available() const { return static_cast<int>(size() - std::min(size(), pos)); }
  bool seek(const size_t p) {
    if (!data || p > size()) return false;
    pos = p;
    return true;
  }
  int read(void* dst, const size_t n) {
    if (!data) return -1;
    const size_t got = pos < size() ? std::min(n, size() - pos) : 0;
    if (got) memcpy(dst, data->bytes.data() + pos, got);
    pos += got;
    return static_cast<int>(got);
  }
  size_t write(const void* src, const size_t n) {
    if (!data) return 0;
    heapcap::Untracked guard;
    if (pos + n > size()) data->bytes.resize(pos + n);
    memcpy(data->bytes.data() + pos, src, n);
    pos += n;
    return n;
  }
  bool close() {
    heapcap::Untracked guard;
    data.reset();
    pos = 0;
    return true;
  }

 private:
  friend struct TestStorage;
  std::shared_ptr<TestFile> data;
  size_t pos = 0;
};

struct TestStorage {
  std::map<std::string, std::shared_ptr<TestFile>> files;

  bool openFileForRead(const char*, const std::string& path, HalFile& out) {
    heapcap::Untracked guard;
    out.close();
    const auto it = files.find(path);
    if (it == files.end()) return false;
    out.data = it->second;
    return true;
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& out) {
    heapcap::Untracked guard;
    out.close();
    out.data = files[path] = std::make_shared<TestFile>();
    return true;
  }
  bool exists(const char* path) const { return files.count(path) != 0; }
  bool remove(const char* path) {
    heapcap::Untracked guard;
    return files.erase(path) != 0;
  }
};

inline TestStorage Storage;
