#pragma once
// Stub for native unit tests — storage ops are no-ops
#include <cstddef>
struct FsFile {};
struct HalStorageClass {
  bool exists(const char*) { return false; }
  bool remove(const char*) { return false; }
  bool openFileForRead(const char*, const char*, FsFile&) { return false; }
  bool openFileForWrite(const char*, const char*, FsFile&) { return false; }
};
inline HalStorageClass Storage;
