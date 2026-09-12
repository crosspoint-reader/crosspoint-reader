#pragma once

#include <Print.h>
#include <StorageTestSupport.h>
#include <common/FsApiConstants.h>

#include <cstring>
#include <utility>
#include <vector>

// Only the SDK seam is simulated. Tests compile the production HalFile and
// HalStorage, including their mutex, ownership and allocation handling.
class FsFile {
  bool opened = false;
  bool directory = false;

 public:
  FsFile() = default;
  explicit FsFile(bool isDirectory) : opened(true), directory(isDirectory) {
    storage_test::requireLock();
    ++storage_test::state.activeHandles;
  }
  FsFile(const FsFile&) = delete;
  FsFile& operator=(const FsFile&) = delete;
  FsFile(FsFile&& other) noexcept : opened(std::exchange(other.opened, false)), directory(other.directory) {}
  FsFile& operator=(FsFile&& other) noexcept {
    close();
    opened = std::exchange(other.opened, false);
    directory = other.directory;
    return *this;
  }
  ~FsFile() { close(); }

  bool isOpen() const { return opened; }
  bool isDirectory() const { return directory; }
  bool close() {
    if (!opened) return true;
    storage_test::requireLock();
    opened = false;
    --storage_test::state.activeHandles;
    ++storage_test::state.closeCalls;
    return storage_test::state.closeSucceeds;
  }
  FsFile openNextFile() {
    storage_test::requireLock();
    ++storage_test::state.nextCalls;
    if (!opened || !directory || storage_test::state.nextEntries == 0) return {};
    --storage_test::state.nextEntries;
    return FsFile(false);
  }

  void flush() {}
  size_t getName(char*, size_t) { return 0; }
  size_t size() { return 0; }
  size_t fileSize() { return 0; }
  bool seekSet(uint64_t) { return opened; }
  bool seekCur(int64_t) { return opened; }
  int available() const { return 0; }
  size_t position() const { return 0; }
  int read(void*, size_t) { return 0; }
  int read() { return -1; }
  size_t write(const void*, size_t count) { return opened ? count : 0; }
  size_t write(uint8_t) { return opened ? 1 : 0; }
  bool rename(const char*) { return opened; }
  void rewindDirectory() {}
};

class SDCardManager {
 public:
  static SDCardManager& getInstance() {
    static SDCardManager instance;
    return instance;
  }
  FsFile open(const char* path, oflag_t = O_RDONLY) {
    storage_test::requireLock();
    ++storage_test::state.openCalls;
    if (!storage_test::state.openSucceeds) return {};
    return FsFile(std::strcmp(path, "/dir") == 0);
  }
  bool openFileForRead(const char*, const char* path, FsFile& file) {
    file = open(path);
    return file.isOpen();
  }
  bool openFileForWrite(const char*, const char* path, FsFile& file) {
    file = open(path);
    return file.isOpen();
  }

  bool begin() { return true; }
  bool ready() const { return true; }
  void shutdown() {}
  std::vector<String> listFiles(const char*, int) { return {}; }
  String readFile(const char*) { return {}; }
  bool readFileToStream(const char*, Print&, size_t) { return false; }
  size_t readFileToBuffer(const char*, char*, size_t, size_t) { return 0; }
  bool writeFile(const char*, const String&) { return false; }
  bool ensureDirectoryExists(const char*) { return false; }
  bool mkdir(const char*, bool) { return false; }
  bool exists(const char*) { return false; }
  bool remove(const char*) { return false; }
  bool rename(const char*, const char*) { return false; }
  bool rmdir(const char*) { return false; }
  bool removeDir(const char*) { return false; }
};
