#pragma once

#include <strings.h>

#include <cstring>
#include <map>
#include <string>

namespace fake {

// FAT compares names case-insensitively.
struct FatLess {
  bool operator()(const std::string& a, const std::string& b) const { return strcasecmp(a.c_str(), b.c_str()) < 0; }
};

// Path -> contents; directories are entries too.
inline std::map<std::string, std::string, FatLess> files;

inline bool within(const std::string& path, const std::string& dir) {
  return strcasecmp(path.c_str(), dir.c_str()) == 0 || (path.size() > dir.size() && path[dir.size()] == '/' &&
                                                        strncasecmp(path.c_str(), dir.c_str(), dir.size()) == 0);
}

}  // namespace fake

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }
  bool exists(const char* path) const { return fake::files.count(path) != 0; }
  bool remove(const char* path) { return fake::files.erase(path) != 0; }
  bool removeDir(const char* path) {
    return std::erase_if(fake::files, [&](const auto& entry) { return fake::within(entry.first, path); }) != 0;
  }
  bool rename(const char* from, const char* to) {
    if (!exists(from) || exists(to)) return false;
    const std::string source = from;
    std::map<std::string, std::string, fake::FatLess> moved;
    for (auto it = fake::files.begin(); it != fake::files.end();) {
      if (fake::within(it->first, source)) {
        moved[to + it->first.substr(source.size())] = it->second;
        it = fake::files.erase(it);
      } else {
        ++it;
      }
    }
    fake::files.merge(moved);
    return true;
  }
};

#define Storage HalStorage::getInstance()
