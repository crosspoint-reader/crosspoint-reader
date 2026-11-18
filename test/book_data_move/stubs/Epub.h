#pragma once

#include <functional>
#include <string>

#include "HalStorage.h"

class Epub {
  std::string cachePath;

 public:
  Epub(const std::string& path, const std::string& cacheDir)
      : cachePath(cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(path))) {}
  const std::string& getCachePath() const { return cachePath; }
  bool clearCache() const { return Storage.removeDir(cachePath.c_str()); }
};
