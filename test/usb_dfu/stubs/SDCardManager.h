#pragma once
#include <algorithm>
#include <cstring>
#include <vector>
extern std::vector<unsigned char> source;
extern bool sourceReadFailure;
struct FsFile {
  size_t pos = 0;
  bool opened = false;
  operator bool() const { return opened; }
  size_t fileSize() const { return source.size(); }
  size_t write(const uint8_t*, size_t n) { return n; }
  void close() { opened = false; }
  int read(void* out, size_t count) {
    if (sourceReadFailure) return -1;
    size_t n = std::min(count, source.size() - pos);
    memcpy(out, source.data() + pos, n);
    pos += n;
    return int(n);
  }
};
struct Storage {
  bool openFileForWrite(const char*, const char*, FsFile& f) {
    f.opened = true;
    return true;
  }
  bool openFileForRead(const char*, const char*, FsFile& f) {
    f.pos = 0;
    f.opened = true;
    return true;
  }
};
inline Storage SdMan;
