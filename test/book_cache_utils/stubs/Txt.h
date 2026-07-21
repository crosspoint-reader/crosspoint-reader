#pragma once

#include <functional>
#include <string>

class Txt {
 public:
  Txt(std::string filepath, const std::string& cacheDir)
      : cachePath_(cacheDir + "/txt_" + std::to_string(std::hash<std::string>{}(filepath))) {}
  void clearCache() {}
  const std::string& getCachePath() const { return cachePath_; }

 private:
  std::string cachePath_;
};
