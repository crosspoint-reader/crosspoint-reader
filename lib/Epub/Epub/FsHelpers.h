#pragma once
#include <string>

class FsHelpers {
 public:
  static bool removeDir(const char* path);
  static std::string normalisePath(const std::string &path);
};
