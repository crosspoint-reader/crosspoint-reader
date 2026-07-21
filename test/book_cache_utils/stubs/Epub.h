#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <functional>
#include <string>

class ZipFile {
 public:
  struct SourceIdentity {
    uint64_t fileSize = 0;
    uint64_t content = 0;

    bool operator==(const SourceIdentity& other) const {
      return fileSize == other.fileSize && content == other.content;
    }
  };

  explicit ZipFile(std::string path) : path_(std::move(path)) {}

  bool getSourceIdentity(SourceIdentity& identity) {
    HalFile file;
    if (!Storage.openFileForRead("EPUB", path_, file)) return false;
    identity.fileSize = file.fileSize64();
    identity.content = 14695981039346656037ULL;
    while (file.available() > 0) {
      uint8_t byte = 0;
      if (file.read(&byte, 1) != 1) {
        file.close();
        return false;
      }
      identity.content = (identity.content ^ byte) * 1099511628211ULL;
    }
    return file.close();
  }

 private:
  std::string path_;
};

class Epub {
 public:
  Epub(std::string filepath, const std::string& cacheDir)
      : cachePath_(cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(filepath))) {}
  void clearCache() {}
  const std::string& getCachePath() const { return cachePath_; }

 private:
  std::string cachePath_;
};
