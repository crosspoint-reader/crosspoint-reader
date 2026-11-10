#pragma once

#include <map>
#include <string>

struct PanelCurveStorageStub {
  std::map<std::string, std::string> files;
  bool canRead = true;

  bool exists(const char* path) const { return files.count(path); }
  bool readFileToString(const char*, const char* path, const size_t limit, std::string& out) const {
    const auto it = files.find(path);
    if (!canRead || it == files.end() || it->second.size() > limit) return false;
    out = it->second;
    return true;
  }
};
inline PanelCurveStorageStub Storage;
