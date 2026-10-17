#pragma once
// In-memory card for FileReplaceTest. Shadows test/shims/HalStorage.h, whose exists() is always
// false. Names compare without regard to case, as on FAT, so one file can be named two ways; a
// rename onto a taken name fails, as SdFat's does (it creates the new entry with O_EXCL); and any
// rename or remove can be made to fail.

#include <cctype>
#include <map>
#include <set>
#include <string>

class HalFile {
 public:
  HalFile() = default;
  HalFile(bool open, bool folder) : open_(open), folder_(folder) {}
  bool isDirectory() const { return folder_; }
  bool close() {
    open_ = false;
    return true;
  }
  operator bool() const { return open_; }

 private:
  bool open_ = false;
  bool folder_ = false;
};

using FsFile = HalFile;

class HalStorage {
 public:
  struct Entry {
    std::string name;  // as spelled when it was made
    std::string content;
    bool folder = false;
  };

  static std::string key(const std::string& path) {
    std::string k;
    k.reserve(path.size());
    for (const char c : path) k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return k;
  }

  void reset() {
    entries.clear();
    failRenameFrom.clear();
    failRemove.clear();
  }
  void put(const std::string& path, const std::string& content) { entries[key(path)] = {path, content, false}; }
  void putFolder(const std::string& path) { entries[key(path)] = {path, "", true}; }
  // Content of the file at `path`, however it is spelled; "<none>" when there is none.
  std::string content(const std::string& path) const {
    const auto it = entries.find(key(path));
    if (it == entries.end()) return "<none>";
    return it->second.content;
  }

  bool exists(const char* path) { return entries.count(key(path)) != 0; }
  HalFile open(const char* path, int = 0) {
    const auto it = entries.find(key(path));
    return it == entries.end() ? HalFile() : HalFile(true, it->second.folder);
  }
  bool rename(const char* from, const char* to) {
    if (failRenameFrom.count(key(from)) != 0) return false;
    const auto it = entries.find(key(from));
    if (it == entries.end() || entries.count(key(to)) != 0) return false;
    Entry moved = it->second;
    moved.name = to;
    entries.erase(it);
    entries[key(to)] = moved;
    return true;
  }
  bool remove(const char* path) {
    if (failRemove.count(key(path)) != 0) return false;
    const auto it = entries.find(key(path));
    if (it == entries.end() || it->second.folder) return false;
    entries.erase(it);
    return true;
  }

  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  std::map<std::string, Entry> entries;  // by key()
  std::set<std::string> failRenameFrom;  // key()s whose rename fails
  std::set<std::string> failRemove;      // key()s whose remove fails
};

#define Storage HalStorage::getInstance()
