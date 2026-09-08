#pragma once

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fake {

struct Node {
  bool directory = false;
  uint32_t time = 1;
  std::vector<uint8_t> bytes;
};

inline std::map<std::string, std::shared_ptr<Node>> files;
inline int failRead = -1;
inline int failWrite = -1;
inline int failRename = -1;
inline int failAlloc = -1;
inline std::string failClosePath;
inline std::string failWritePath;
inline unsigned parses = 0;
inline unsigned reads = 0;
inline unsigned seeks = 0;
inline unsigned delays = 0;
inline std::map<std::string, unsigned> writesByPath;
inline std::map<std::string, unsigned> directoryEntriesByPath;
inline bool failureTriggered = false;
inline std::map<std::string, std::vector<std::string>> extraDirectoryEntries;

inline bool fail(int& count) {
  if (count < 0) return false;
  if (count == 0) {
    count = -1;
    failureTriggered = true;
    return true;
  }
  --count;
  return false;
}

inline void reset() {
  files.clear();
  failRead = -1;
  failWrite = -1;
  failRename = -1;
  failAlloc = -1;
  failClosePath.clear();
  failWritePath.clear();
  parses = 0;
  reads = 0;
  seeks = 0;
  delays = 0;
  writesByPath.clear();
  directoryEntriesByPath.clear();
  failureTriggered = false;
  extraDirectoryEntries.clear();
}

inline void resetIoCounters() {
  reads = 0;
  seeks = 0;
  delays = 0;
}

inline void add(const std::string& path, const std::string& bytes = "book", const uint32_t time = 1) {
  auto node = std::make_shared<Node>();
  node->time = time;
  node->bytes.assign(bytes.begin(), bytes.end());
  files[path] = node;
  std::string parent = path.substr(0, path.find_last_of('/'));
  if (parent.empty()) parent = "/";
  if (!files.count(parent)) {
    add(parent, "");
    files[parent]->directory = true;
  }
}

inline void duplicateDirectoryEntry(const std::string& path) {
  std::string parent = path.substr(0, path.find_last_of('/'));
  if (parent.empty()) parent = "/";
  extraDirectoryEntries[parent].push_back(path);
}

}  // namespace fake

class HalFile {
 public:
  std::shared_ptr<fake::Node> node;
  std::string path;
  size_t pos = 0;

  explicit operator bool() const { return bool(node); }
  bool isOpen() const { return bool(node); }
  bool close() {
    const bool failed = !fake::failClosePath.empty() && path == fake::failClosePath;
    if (failed) {
      fake::failClosePath.clear();
      fake::failureTriggered = true;
    }
    node.reset();
    return !failed;
  }
  bool isDirectory() const { return node && node->directory; }
  void rewindDirectory() { pos = 0; }
  HalFile openNextFile() {
    std::vector<std::string> children;
    for (const auto& [name, value] : fake::files) {
      std::string parent = name.substr(0, name.find_last_of('/'));
      if (parent.empty()) parent = "/";
      if (name != path && parent == path) children.push_back(name);
    }
    const auto extras = fake::extraDirectoryEntries.find(path);
    if (extras != fake::extraDirectoryEntries.end()) {
      children.insert(children.end(), extras->second.begin(), extras->second.end());
    }
    if (pos >= children.size()) return {};
    HalFile file;
    file.path = children[pos++];
    file.node = fake::files[file.path];
    fake::directoryEntriesByPath[file.path]++;
    return file;
  }
  size_t getName(char* out, const size_t size) {
    const std::string name = path.substr(path.find_last_of('/') + 1);
    if (size > 0) {
      std::strncpy(out, name.c_str(), size);
      out[size - 1] = '\0';
    }
    return name.size();
  }
  uint32_t modificationTime() const { return node ? node->time : 0; }
  uint64_t fileSize64() const { return node ? node->bytes.size() : 0; }
  size_t fileSize() const { return static_cast<size_t>(fileSize64()); }
  size_t position() const { return pos; }
  bool seekSet(const size_t offset) {
    fake::seeks++;
    if (!node || (!node->directory && offset > node->bytes.size())) return false;
    pos = offset;
    return true;
  }
  bool seek(const size_t offset) { return seekSet(offset); }
  int read(void* out, size_t size) {
    fake::reads++;
    if (size == 0) return 0;
    if (!node || fake::fail(fake::failRead)) return -1;
    size = std::min(size, node->bytes.size() - std::min(pos, node->bytes.size()));
    std::memcpy(out, node->bytes.data() + std::min(pos, node->bytes.size()), size);
    pos += size;
    return static_cast<int>(size);
  }
  size_t write(const uint8_t* data, const size_t size) {
    if (size == 0) return 0;
    fake::writesByPath[path]++;
    if (!fake::failWritePath.empty() && path == fake::failWritePath) {
      fake::failWritePath.clear();
      fake::failureTriggered = true;
      return 0;
    }
    if (!node || fake::fail(fake::failWrite)) return 0;
    node->bytes.resize(std::max(node->bytes.size(), pos + size));
    std::memcpy(node->bytes.data() + pos, data, size);
    pos += size;
    return size;
  }
  size_t write(const void* data, const size_t size) { return write(static_cast<const uint8_t*>(data), size); }
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }

  bool exists(const char* path) const { return fake::files.count(path) != 0; }
  bool mkdir(const char* path) {
    if (!exists(path)) fake::add(path, "");
    fake::files[path]->directory = true;
    return true;
  }
  HalFile open(const char* path) {
    HalFile file;
    const auto found = fake::files.find(path);
    if (found != fake::files.end()) {
      file.node = found->second;
      file.path = path;
    }
    return file;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    file = open(path);
    return bool(file);
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    fake::add(path, "");
    file = open(path);
    return true;
  }
  bool openFileForWrite(const char* module, const std::string& path, HalFile& file) {
    return openFileForWrite(module, path.c_str(), file);
  }
  bool remove(const char* path) { return fake::files.erase(path) != 0; }
  bool rename(const char* from, const char* to) {
    if (fake::fail(fake::failRename) || !exists(from) || exists(to)) return false;
    fake::files[to] = fake::files[from];
    fake::files.erase(from);
    return true;
  }
};

#define Storage HalStorage::getInstance()
