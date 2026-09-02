#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class HalFile {
  friend class HalStorage;

 public:
  bool close() {
    if (!hasImpl) {
      invalidCloses++;
      return false;
    }
    open = false;
    return true;
  }

  bool isOpen() const { return hasImpl && open; }
  int read(void*, size_t) { return 0; }
  uint64_t fileSize64() { return 0; }
  bool seekSet(size_t) { return false; }

  static void resetInvalidCloseCount() { invalidCloses = 0; }
  static int invalidCloseCount() { return invalidCloses; }

 private:
  bool hasImpl = false;
  bool open = false;
  static inline int invalidCloses = 0;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool openFileForRead(const char*, const char*, HalFile& file) {
    file.hasImpl = true;
    file.open = false;
    return false;
  }
};

#define Storage HalStorage::getInstance()
