#pragma once

#include <SdFat.h>
#include <WString.h>

#include <string>
#include <vector>

class SDCardManager {
 public:
  SDCardManager();
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

  FsFile open(const char* path, const oflag_t oflag = O_RDONLY) { return sd.open(path, oflag); }
  bool mkdir(const char* path, const bool pFlag = true) { return sd.mkdir(path, pFlag); }
  bool exists(const char* path) { return sd.exists(path); }
  bool remove(const char* path) { return sd.remove(path); }
  bool rmdir(const char* path) { return sd.rmdir(path); }
  bool rename(const char* path, const char* newPath) { return sd.rename(path, newPath); }

  bool openFileForRead(const char* moduleName, const char* path, FsFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, FsFile& file);
  bool openFileForRead(const char* moduleName, const String& path, FsFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, FsFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, FsFile& file);
  bool openFileForWrite(const char* moduleName, const String& path, FsFile& file);
  bool removeDir(const char* path);

  static SDCardManager& getInstance() { return instance; }

 private:
  static SDCardManager instance;

  bool initialized = false;
  SdFat sd;
};

#define SdMan SDCardManager::getInstance()
