#pragma once
#include <FS.h>

class FsHelpers {
 public:
  static bool openFileForRead(const char* moduleName, const char* path, File& file);
  static bool openFileForRead(const char* moduleName, const std::string& path, File& file);
  static bool openFileForRead(const char* moduleName, const String& path, File& file);
  static bool openFileForWrite(const char* moduleName, const char* path, File& file);
  static bool openFileForWrite(const char* moduleName, const std::string& path, File& file);
  static bool openFileForWrite(const char* moduleName, const String& path, File& file);
  static bool removeDir(const char* path);
  static std::string normalisePath(const std::string& path);
};
