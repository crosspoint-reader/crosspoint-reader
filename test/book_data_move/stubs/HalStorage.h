#pragma once

#include <common/FsUtf.h>
#include <common/upcase.h>

#include <cstring>
#include <map>
#include <string>

namespace fake {

// FAT long names match after SdFat's own UTF-16 case fold.
inline std::u16string fold(const std::string& path) {
  std::u16string out;
  for (const char *p = path.c_str(), *end = p + path.size(); p && p < end;) {
    uint16_t hi, lo;
    p = FsUtf::mbToU16(p, end, &hi, &lo);
    if (p) out += {static_cast<char16_t>(toUpcase(hi)), static_cast<char16_t>(toUpcase(lo))};
  }
  return out;
}

struct FatLess {
  bool operator()(const std::string& a, const std::string& b) const { return fold(a) < fold(b); }
};

// Path -> contents; a directory holds kDir.
inline constexpr const char* kDir = "<dir>";
inline std::map<std::string, std::string, FatLess> files;

inline bool within(const std::string& path, const std::string& dir) {
  return fold(path) == fold(dir) || fold(path).starts_with(fold(dir + "/"));
}

}  // namespace fake

struct HalFile {
  bool directory = false;
  bool isDirectory() const { return directory; }
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }
  HalFile open(const char* path) const {
    const auto it = fake::files.find(path);
    return {it != fake::files.end() && it->second == fake::kDir};
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
