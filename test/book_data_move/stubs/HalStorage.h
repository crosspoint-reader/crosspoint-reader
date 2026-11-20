#pragma once

#include <common/FsUtf.h>
#include <common/upcase.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

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

// Mirrors the real HalFile surface BookCacheUtils uses; an open file is a snapshot of its direct children.
struct HalFile {
  bool valid = false;
  bool directory = false;
  std::string path;
  std::vector<std::string> children;  // absolute paths of direct children
  size_t next = 0;

  bool isDirectory() const { return valid && directory; }
  explicit operator bool() const { return valid; }
  bool close() {
    valid = false;
    return true;
  }
  size_t getName(char* name, size_t len) const {
    if (!len) return 0;
    const size_t slash = path.rfind('/');
    const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t n = std::min(base.size(), len - 1);
    memcpy(name, base.data(), n);
    name[n] = '\0';
    return n;
  }
  HalFile openNextFile();
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }
  HalFile open(const char* path) const {
    const auto it = fake::files.find(path);
    if (it == fake::files.end()) return {};
    HalFile file{true, it->second == fake::kDir, it->first, {}, 0};
    if (file.directory) {
      const std::u16string prefix = fake::fold(file.path + "/");
      for (const auto& entry : fake::files) {
        const std::u16string folded = fake::fold(entry.first);
        if (folded.starts_with(prefix) && folded.find(u'/', prefix.size()) == std::u16string::npos) {
          file.children.push_back(entry.first);
        }
      }
    }
    return file;
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

inline HalFile HalFile::openNextFile() {
  if (!isDirectory()) return {};
  while (next < children.size()) {
    const std::string child = children[next++];
    if (fake::files.count(child)) return HalStorage::getInstance().open(child.c_str());
  }
  return {};
}

#define Storage HalStorage::getInstance()
