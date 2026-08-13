#pragma once

#include <cstring>
#include <map>
#include <string>
#include <vector>

struct FakeStorageNode {
  std::string name;
  bool directory = false;
  bool validBitmap = false;
  std::vector<FakeStorageNode*> children;
};

class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(FakeStorageNode* node) : node(node) {}
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  explicit operator bool() const { return node != nullptr; }
  bool isDirectory() const { return node && node->directory; }
  bool hasValidBitmap() const { return node && !node->directory && node->validBitmap; }

  size_t getName(char* destination, const size_t capacity) const {
    if (!node || !destination || capacity <= node->name.size()) {
      if (destination && capacity > 0) destination[0] = '\0';
      return 0;
    }
    memcpy(destination, node->name.c_str(), node->name.size() + 1);
    return node->name.size();
  }

  HalFile openNextFile() {
    if (!node || !node->directory || nextChild >= node->children.size()) return {};
    return HalFile(node->children[nextChild++]);
  }

 private:
  FakeStorageNode* node = nullptr;
  size_t nextChild = 0;
};

class FakeStorage {
 public:
  static FakeStorage& getInstance() {
    static FakeStorage instance;
    return instance;
  }

  void reset() {
    nodes.clear();
    addDirectory("/");
  }

  void addDirectory(const std::string& path) { ensureNode(normalize(path), true, false); }

  void addFile(const std::string& path, const bool validBitmap = true) {
    ensureNode(normalize(path), false, validBitmap);
  }

  HalFile open(const char* path) {
    if (!path) return {};
    const auto it = nodes.find(normalize(path));
    return it == nodes.end() ? HalFile{} : HalFile(&it->second);
  }

 private:
  FakeStorage() { reset(); }

  static std::string normalize(std::string path) {
    if (path.empty()) return "/";
    if (path.front() != '/') path.insert(path.begin(), '/');
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
  }

  static std::string parentPath(const std::string& path) {
    const size_t separator = path.find_last_of('/');
    return separator == 0 ? "/" : path.substr(0, separator);
  }

  static std::string baseName(const std::string& path) {
    if (path == "/") return "/";
    return path.substr(path.find_last_of('/') + 1);
  }

  FakeStorageNode* ensureNode(const std::string& path, const bool directory, const bool validBitmap) {
    const auto existing = nodes.find(path);
    if (existing != nodes.end()) {
      existing->second.directory = directory;
      existing->second.validBitmap = validBitmap;
      return &existing->second;
    }

    FakeStorageNode* parent = nullptr;
    if (path != "/") parent = ensureNode(parentPath(path), true, false);

    auto it = nodes.emplace(path, FakeStorageNode{baseName(path), directory, validBitmap, {}}).first;
    FakeStorageNode* const rawNode = &it->second;
    if (parent) parent->children.push_back(rawNode);
    return rawNode;
  }

  std::map<std::string, FakeStorageNode> nodes;
};

#define Storage FakeStorage::getInstance()
