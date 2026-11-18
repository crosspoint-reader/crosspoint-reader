#pragma once

#include <functional>
#include <string>

#include "HalStorage.h"

class Xtc {
  std::string cachePath;

 public:
  Xtc(const std::string& path, const std::string& cacheDir)
      : cachePath(cacheDir + "/xtc_" + std::to_string(std::hash<std::string>{}(path))) {}
  std::string getCachePath() const { return cachePath; }
  bool clearCache() const { return Storage.removeDir(cachePath.c_str()); }
};
