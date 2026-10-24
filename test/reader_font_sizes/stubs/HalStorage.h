#pragma once

#include <cstddef>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

// In-memory directory tree for SdCardFontRegistry::discover(): directory path
// -> its entries as (name, isDirectory). Plain files are only listed.
inline std::map<std::string, std::vector<std::pair<std::string, bool>>> fakeSdDirs;

class HalFile {
 public:
  HalFile() = default;
  HalFile(std::string path, std::string name, const bool directory)
      : path_(std::move(path)), name_(std::move(name)), directory_(directory), valid_(true) {}

  explicit operator bool() const { return valid_; }
  bool isDirectory() const { return directory_; }
  void close() {}
  void getName(char* buffer, const size_t size) const { snprintf(buffer, size, "%s", name_.c_str()); }

  HalFile openNextFile() {
    const auto dir = fakeSdDirs.find(path_);
    if (dir == fakeSdDirs.end() || next_ >= dir->second.size()) return {};
    const auto& [name, directory] = dir->second[next_++];
    return {path_ + "/" + name, name, directory};
  }

 private:
  std::string path_;
  std::string name_;
  bool directory_ = false;
  bool valid_ = false;
  size_t next_ = 0;
};

class HalStorage {
 public:
  HalFile open(const char* path) const {
    if (!exists(path)) return {};
    return {path, path, true};
  }
  bool exists(const char* path) const { return fakeSdDirs.count(path) != 0; }
};

inline HalStorage Storage;
