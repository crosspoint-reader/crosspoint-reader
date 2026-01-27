#pragma once

#include <Arduino.h>
#include <vector>

#if CROSSPOINT_EMULATED == 0
#include <SDCardManager.h>
#else
typedef int oflag_t;
#define	O_RDONLY	0		/* +1 == FREAD */
#define	O_WRONLY	1		/* +1 == FWRITE */
#define	O_RDWR		2		/* +1 == FREAD|FWRITE */

class FsFile : public Print {
  String path;
  String name;
  oflag_t oflag; // unused for now
  bool open = false;
  // directory state
  bool isDir = false;
  std::vector<String> dirEntries;
  size_t dirIndex = 0;
  // file state
  size_t filePos = 0;
  size_t fileSizeBytes = 0;
 public:
  FsFile() = default;
  FsFile(const char* path, oflag_t oflag);
  ~FsFile() = default;

  void flush() { /* no-op */ }
  size_t getName(char* name, size_t len) {
    String n = path;
    if (n.length() >= len) {
      n = n.substring(0, len - 1);
    }
    n.toCharArray(name, len);
    return n.length();
  }
  size_t size() { return fileSizeBytes; }
  size_t fileSize() { return size(); }
  size_t seek(size_t pos) { 
    filePos = pos; 
    return filePos; 
  }
  size_t seekCur(int64_t offset) { return seek(filePos + offset); }
  size_t seekSet(size_t offset) { return seek(offset); }
  int available() const { return (fileSizeBytes > filePos) ? (fileSizeBytes - filePos) : 0; }
  size_t position() const { return filePos; }
  int read(void* buf, size_t count);
  int read(); // read a single byte
  size_t write(const uint8_t* buffer, size_t size);
  size_t write(uint8_t b) override;
  bool isDirectory() const { return isDir; }
  int rewindDirectory() {
    if (!isDir) return -1;
    dirIndex = 0;
    return 0;
  }
  bool close() { open = false; return true; }
  FsFile openNextFile() {
    if (!isDir || dirIndex >= dirEntries.size()) {
      return FsFile();
    }
    FsFile f(dirEntries[dirIndex].c_str(), O_RDONLY);
    dirIndex++;
    return f;
  }
  bool isOpen() const { return open; }
  operator bool() const { return isOpen(); }
};
#endif

class HalStorage {
 public:
  HalStorage();
  bool begin();
  bool ready() const;
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200);
  // Read the entire file at `path` into a String. Returns empty string on failure.
  String readFile(const char* path);
  // Low-memory helpers:
  // Stream the file contents to a `Print` (e.g. `Serial`, or any `Print`-derived object).
  // Returns true on success, false on failure.
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256);
  // Read up to `bufferSize-1` bytes into `buffer`, null-terminating it. Returns bytes read.
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes = 0);
  // Write a string to `path` on the SD card. Overwrites existing file.
  // Returns true on success.
  bool writeFile(const char* path, const String& content);
  // Ensure a directory exists, creating it if necessary. Returns true on success.
  bool ensureDirectoryExists(const char* path);

  FsFile open(const char* path, const oflag_t oflag = O_RDONLY);
  bool mkdir(const char* path, const bool pFlag = true);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rmdir(const char* path);

  bool openFileForRead(const char* moduleName, const char* path, FsFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, FsFile& file);
  bool openFileForRead(const char* moduleName, const String& path, FsFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, FsFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, FsFile& file);
  bool openFileForWrite(const char* moduleName, const String& path, FsFile& file);
  bool removeDir(const char* path);

 static HalStorage& getInstance() { return instance; }

 private:
  static HalStorage instance;

  bool is_emulated = CROSSPOINT_EMULATED;
};

// TODO @ngxson : this is a trick to avoid changing too many files at once.
// consider refactoring in a dedicated PR later.
#if CROSSPOINT_EMULATED == 1
#define SdMan HalStorage::getInstance()
#endif
