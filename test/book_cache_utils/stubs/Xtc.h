#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <functional>
#include <string>

class Xtc {
 public:
  Xtc(std::string filepath, const std::string& cacheDir)
      : filepath_(std::move(filepath)),
        cachePath_(cacheDir + "/xtc_" + std::to_string(std::hash<std::string>{}(filepath_))) {}
  bool load() {
    constexpr uint32_t XTC_MAGIC = 0x00435458;
    constexpr uint32_t XTCH_MAGIC = 0x48435458;
    HalFile file;
    uint32_t magic = 0;
    if (!Storage.openFileForRead("XTC", filepath_, file) ||
        file.read(&magic, sizeof(magic)) != static_cast<int>(sizeof(magic))) {
      if (file) file.close();
      return false;
    }
    const bool closed = file.close();
    return closed && (magic == XTC_MAGIC || magic == XTCH_MAGIC);
  }
  void clearCache() {}
  const std::string& getCachePath() const { return cachePath_; }

 private:
  std::string filepath_;
  std::string cachePath_;
};
