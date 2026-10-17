#pragma once
// In-memory HalStorage for KOReaderDocumentId: one book file whose seeks and reads a test can
// fail at chosen offsets, plus a map standing in for the SD card's other files (the hash cache).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Print.h"
#include "WString.h"

namespace kodoc_fixture {

constexpr const char* kBookPath = "/books/book.epub";

struct State {
  std::vector<uint8_t> book;                 // contents of kBookPath
  long failSeekTo = -1;                      // seekSet() to this offset fails
  long faultReadAt = -1;                     // a read starting here returns faultReadResult instead
  int faultReadResult = -1;                  // -1: I/O error; 0..n-1: a short read of that many bytes
  std::map<std::string, std::string> files;  // every other file (the hash cache)
  int openFiles = 0;
};

inline State state;

}  // namespace kodoc_fixture

class HalFile : public Print {
 public:
  HalFile() = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  ~HalFile() { close(); }

  bool openBook() {
    open_ = true;
    pos_ = 0;
    ++kodoc_fixture::state.openFiles;
    return true;
  }
  bool close() {
    if (open_) --kodoc_fixture::state.openFiles;
    open_ = false;
    return true;
  }

  size_t fileSize() const { return open_ ? book().size() : 0; }
  bool getModifyDateTime(uint16_t* pdate, uint16_t* ptime) const {
    *pdate = 0x5A21;
    *ptime = 0x7B3C;
    return open_;
  }
  bool seekSet(size_t pos) {
    if (!open_ || static_cast<long>(pos) == kodoc_fixture::state.failSeekTo || pos > book().size()) return false;
    pos_ = pos;
    return true;
  }
  int read(void* buf, size_t n) {
    if (!open_) return -1;
    size_t take = std::min(n, book().size() - pos_);
    if (static_cast<long>(pos_) == kodoc_fixture::state.faultReadAt) {
      if (kodoc_fixture::state.faultReadResult < 0) return kodoc_fixture::state.faultReadResult;
      take = std::min(take, static_cast<size_t>(kodoc_fixture::state.faultReadResult));
    }
    std::memcpy(buf, book().data() + pos_, take);
    pos_ += take;
    return static_cast<int>(take);
  }

  size_t write(uint8_t) override { return 0; }
  size_t write(const uint8_t*, size_t) override { return 0; }

 private:
  static const std::vector<uint8_t>& book() { return kodoc_fixture::state.book; }
  bool open_ = false;
  size_t pos_ = 0;
};

using FsFile = HalFile;

class HalStorage {
 public:
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    return path == kodoc_fixture::kBookPath && file.openBook();
  }
  bool exists(const char* path) const { return kodoc_fixture::state.files.count(path) != 0; }
  String readFile(const char* path) const {
    const auto it = kodoc_fixture::state.files.find(path);
    return it == kodoc_fixture::state.files.end() ? String() : String(it->second.c_str());
  }
  bool writeFile(const char* path, const String& content) {
    kodoc_fixture::state.files[path] = content.c_str();
    return true;
  }
  bool ensureDirectoryExists(const char*) { return true; }

  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
};

#define Storage HalStorage::getInstance()
