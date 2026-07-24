#pragma once

#include <functional>
#include <string>
#include <utility>

class Txt {
 public:
  Txt(std::string filepath, const std::string& cacheDir)
      : filepath_(std::move(filepath)),
        cachePath_(cacheDir + "/txt_" + std::to_string(std::hash<std::string>{}(filepath_))) {}
  bool load() const { return filepath_ != rejectedLoadPath_; }
  void clearCache() {}
  const std::string& getCachePath() const { return cachePath_; }
  static void rejectLoadForTest(std::string path) { rejectedLoadPath_ = std::move(path); }
  static void clearRejectedLoadForTest() { rejectedLoadPath_.clear(); }

 private:
  inline static std::string rejectedLoadPath_;
  std::string filepath_;
  std::string cachePath_;
};
