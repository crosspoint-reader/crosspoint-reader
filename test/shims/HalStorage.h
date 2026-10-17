#pragma once
// Minimal stub for host/test builds — only the types needed to compile
// CssParser.h and CssStyle.h without ESP32/SdFat dependencies.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Print.h"
#include "WString.h"

// Minimal FsFile stub — only the methods CssParser calls at runtime are needed.
// The default (no-backing-store) instance stubs all I/O as no-ops/failures.
// Use HalFile::fromString() to get a readable instance backed by a CSS string —
// used by tests that call CssParser::loadFromStream().
class HalFile : public Print {
 public:
  HalFile() = default;
  ~HalFile() = default;
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  // Returns a readable HalFile backed by the given string content.
  static HalFile fromString(std::string content) {
    HalFile f;
    f.backing_ = std::move(content);
    f.hasData_ = true;
    f.hasImpl_ = true;
    return f;
  }

  // Returns an empty read/write HalFile backed by an in-memory buffer — used by
  // serialization round-trip tests (write, seek(0), read back). Default-constructed
  // instances keep their historical no-op write semantics.
  static HalFile forReadWrite() {
    HalFile f;
    f.hasData_ = true;
    f.writable_ = true;
    f.hasImpl_ = true;
    return f;
  }

  // A handle HalStorage tried to open. Device parity: HalStorage hands out an Impl even when the
  // open FAILED, so close() is legal on it — unlike a handle nobody ever opened. The stubs below
  // return one from every open call for exactly that reason.
  static HalFile opened() {
    HalFile f;
    f.hasImpl_ = true;
    return f;
  }

  void flush() {}
  size_t getName(char*, size_t) { return 0; }
  size_t size() { return hasData_ ? backing_.size() : 0; }
  size_t fileSize() { return size(); }
  bool seek(size_t pos) {
    if (!hasData_) return false;
    pos_ = pos < backing_.size() ? pos : backing_.size();
    return true;
  }
  bool seekCur(int64_t) { return false; }
  bool seekSet(size_t pos) { return seek(pos); }
  int available() const { return hasData_ ? static_cast<int>(backing_.size() - pos_) : 0; }
  size_t position() const { return pos_; }
  int read(void* buf, size_t n) {
    if (!hasData_) return -1;
    const size_t remaining = backing_.size() - pos_;
    if (remaining == 0) return 0;
    const size_t toRead = n < remaining ? n : remaining;
    std::memcpy(buf, backing_.data() + pos_, toRead);
    pos_ += toRead;
    return static_cast<int>(toRead);
  }
  int read() { return -1; }
  uint64_t size64() { return size(); }
  uint64_t fileSize64() { return size(); }
  bool seek64(uint64_t) { return false; }
  bool seekSet64(uint64_t) { return false; }
  uint64_t position64() const { return pos_; }
  size_t write(const void* buf, size_t n) {
    if (!writable_) return 0;
    const auto* bytes = static_cast<const char*>(buf);
    if (pos_ < backing_.size()) {
      const size_t overlap = std::min(n, backing_.size() - pos_);
      std::memcpy(&backing_[pos_], bytes, overlap);
      backing_.append(bytes + overlap, n - overlap);
    } else {
      backing_.append(bytes, n);
    }
    pos_ += n;
    return n;
  }
  size_t write(const uint8_t* buf, size_t n) override { return write(static_cast<const void*>(buf), n); }
  size_t write(uint8_t b) override { return write(static_cast<const void*>(&b), 1); }
  bool rename(const char*) { return false; }
  bool getModifyDateTime(uint16_t*, uint16_t*) { return false; }
  bool isDirectory() const { return false; }
  void rewindDirectory() {}
  // Device parity, deliberately fatal: HalFile::close() asserts on a handle that was never opened
  // (impl == nullptr), so closing one must fail here too — otherwise the bug is invisible on the
  // host and aborts in the reader's hands. Not assert(): host tests build with NDEBUG.
  bool close() {
    if (!hasImpl_) {
      std::fprintf(stderr, "HalFile::close() on a handle that was never opened — this aborts on device\n");
      std::abort();
    }
    return false;
  }
  HalFile openNextFile() { return opened(); }
  bool isOpen() const { return hasData_; }
  operator bool() const { return hasData_; }

 private:
  std::string backing_;
  size_t pos_ = 0;
  bool hasData_ = false;
  bool writable_ = false;
  bool hasImpl_ = false;
};

using FsFile = HalFile;

class HalStorage {
 public:
  bool begin() { return false; }
  bool ready() const { return false; }
  std::vector<String> listFiles(const char* = "/", int = 200) { return {}; }
  String readFile(const char*) { return ""; }
  bool readFileToStream(const char*, Print&, size_t = 256) { return false; }
  size_t readFileToBuffer(const char*, char*, size_t, size_t = 0) { return 0; }
  bool readFileToString(const char*, const std::string&, size_t, std::string& out) {
    out.clear();
    return false;
  }
  bool writeFile(const char*, const String&) { return false; }
  bool ensureDirectoryExists(const char*) { return false; }
  HalFile open(const char*, int = 0) { return HalFile::opened(); }
  bool mkdir(const char*, bool = true) { return false; }
  bool exists(const char*) { return false; }
  bool remove(const char*) { return false; }
  bool rename(const char*, const char*) { return false; }
  bool rmdir(const char*) { return false; }
  bool openFileForRead(const char*, const char*, HalFile& file) {
    file = HalFile::opened();
    return false;
  }
  bool openFileForRead(const char*, const std::string&, HalFile& file) {
    file = HalFile::opened();
    return false;
  }
  bool openFileForRead(const char*, const String&, HalFile& file) {
    file = HalFile::opened();
    return false;
  }
  bool openFileForUpdate(const char*, const char*, HalFile& file) {
    file = HalFile::opened();
    return false;
  }
  bool openFileForUpdate(const char*, const std::string&, HalFile& file) {
    file = HalFile::opened();
    return false;
  }
  bool openFileForWrite(const char*, const char*, HalFile& file) {
    file = HalFile::opened();
    return false;
  }
  bool openFileForWrite(const char*, const std::string&, HalFile& file) {
    file = HalFile::opened();
    return false;
  }
  bool openFileForWrite(const char*, const String&, HalFile& file) {
    file = HalFile::opened();
    return false;
  }
  bool removeDir(const char*) { return false; }
  bool copyFile(const char*, const std::string&, const char*) { return false; }
  uint64_t sdTotalBytes() const { return 0; }
  uint64_t sdUsedBytes() { return 0; }
  uint64_t sdFreeBytes() { return 0; }
  static HalStorage& getInstance() {
    static HalStorage inst;
    return inst;
  }
};

#define Storage HalStorage::getInstance()
