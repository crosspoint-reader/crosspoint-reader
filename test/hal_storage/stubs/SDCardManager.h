// Host stand-in for the SDK's SDCardManager: the calls HalStorage.cpp makes, on an FsVolume the test
// mounts over an in-memory card (HalStorageTest.cpp). Those the tests do not reach answer as an
// empty card would.
#pragma once
#include <Print.h>
#include <SdFat.h>
#include <WString.h>

#include <vector>

class SDCardManager {
 public:
  bool begin() { return vol != nullptr; }
  bool ready() const { return vol != nullptr; }
  void prepareForSleep() {}
  uint64_t sdTotalBytes() const { return 0; }
  uint64_t sdUsedBytes() { return 0; }
  std::vector<String> listFiles(const char*, int) { return {}; }
  String readFile(const char*) { return String(); }
  bool readFileToStream(const char*, Print&, size_t) { return false; }
  size_t readFileToBuffer(const char*, char*, size_t, size_t) { return 0; }
  bool writeFile(const char*, const String&) { return false; }
  bool ensureDirectoryExists(const char* path) { return vol->exists(path) || vol->mkdir(path, true); }

  FsFile open(const char* path, const oflag_t oflag = O_RDONLY) { return vol->open(path, oflag); }
  bool mkdir(const char* path, const bool pFlag = true) { return vol->mkdir(path, pFlag); }
  bool exists(const char* path) { return vol->exists(path); }
  bool remove(const char* path) { return vol->remove(path); }
  bool rmdir(const char* path) { return vol->rmdir(path); }
  bool rename(const char* path, const char* newPath) { return vol->rename(path, newPath); }
  // As the SDK's: a failed open answers false and leaves `file` closed.
  bool openFileForRead(const char*, const char* path, FsFile& file) {
    file = vol->open(path, O_RDONLY);
    return file.isOpen();
  }
  bool openFileForWrite(const char*, const char* path, FsFile& file) {
    file = vol->open(path, O_RDWR | O_CREAT | O_TRUNC);
    return file.isOpen();
  }
  bool removeDir(const char*) { return false; }

  static SDCardManager& getInstance() {
    static SDCardManager instance;
    return instance;
  }

  FsVolume* vol = nullptr;  // mounted by the test
};
