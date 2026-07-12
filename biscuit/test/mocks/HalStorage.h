#pragma once
#include <cstdio>
#include <cstdint>
#include <string>

#define O_WRITE 0x02
#define O_CREAT 0x04
#define O_APPEND 0x08
#define O_TRUNC 0x10

class FsFile {
 public:
  operator bool() const { return false; }
  bool isDirectory() { return false; }
  void close() {}
  void getName(char* buf, int len) { buf[0] = '\0'; }
  int read(uint8_t*, int) { return 0; }
  void write(const uint8_t*, int) {}
  void println(const char*) {}
  void println(const std::string& s) { println(s.c_str()); }
  void print(const char*) {}
  bool available() { return false; }
  char read() { return 0; }
  size_t size() { return 0; }
  void rewindDirectory() {}
  FsFile openNextFile() { return FsFile(); }
};

struct StorageMock {
  void mkdir(const char*) {}
  bool exists(const char*) { return false; }
  FsFile open(const char*, int = 0) { return FsFile(); }
  bool remove(const char*) { return false; }
  bool removeDir(const char*) { return false; }
  bool openFileForRead(const char*, const std::string&, FsFile&) { return false; }
  bool openFileForWrite(const char*, const std::string&, FsFile&) { return false; }
  void writeFile(const char*, const std::string&) {}
};

inline StorageMock Storage;
