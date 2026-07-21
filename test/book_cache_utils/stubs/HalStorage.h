#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

class HalStorage;

class HalFile {
 public:
  HalFile() = default;
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool isDirectory() const { return open_ && directory_; }
  size_t getName(char* destination, size_t capacity) const;
  HalFile openNextFile();
  uint64_t fileSize64() const;
  size_t available() const { return open_ && !directory_ ? static_cast<size_t>(fileSize64()) - position_ : 0; }
  int read(void* destination, size_t length);
  size_t write(const void* source, size_t length);
  void flush() {}
  bool sync();
  bool close() {
    const bool wasOpen = open_;
    open_ = false;
    return wasOpen;
  }
  explicit operator bool() const { return open_; }

 private:
  friend class HalStorage;

  struct Entry {
    std::string name;
    bool directory = false;
  };

  bool open_ = false;
  bool directory_ = false;
  bool writable_ = false;
  HalStorage* storage_ = nullptr;
  std::string path_;
  size_t position_ = 0;
  std::string name_;
  std::vector<Entry> entries_;
  size_t nextEntry_ = 0;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool exists(const char* path) const { return files_.count(path) != 0 || directories_.count(path) != 0; }
  bool exists(const std::string& path) const { return exists(path.c_str()); }

  bool mkdir(const char* path, bool = true) {
    if (exists(path)) return false;
    const std::string parent = parentPath(path);
    if (!parent.empty() && directories_.count(parent) == 0) return false;
    directories_.insert(path);
    return true;
  }

  HalFile open(const char* path) const {
    HalFile file;
    const auto directory = directories_.find(path);
    const auto storedFile = files_.find(path);
    if (directory == directories_.end() && storedFile == files_.end()) return file;

    file.open_ = true;
    file.directory_ = directory != directories_.end();
    file.name_ = baseName(path);
    if (file.directory_) file.entries_ = immediateChildren(path);
    return file;
  }

  bool remove(const char* path) {
    if (shouldFailDelete(path)) return false;
    return files_.erase(path) != 0;
  }
  bool remove(const std::string& path) { return remove(path.c_str()); }

  bool removeDir(const char* path) {
    if (shouldFailDelete(path) || directories_.count(path) == 0) return false;
    const std::string prefix = withTrailingSlash(path);
    for (auto file = files_.begin(); file != files_.end();) {
      file = file->first.compare(0, prefix.size(), prefix) == 0 ? files_.erase(file) : std::next(file);
    }
    for (auto directory = directories_.begin(); directory != directories_.end();) {
      if (*directory == path || directory->compare(0, prefix.size(), prefix) == 0) {
        directory = directories_.erase(directory);
      } else {
        ++directory;
      }
    }
    return true;
  }

  bool rmdir(const char* path) {
    if (failRmdirOnce_) {
      failRmdirOnce_ = false;
      return false;
    }
    if (directories_.count(path) == 0 || !immediateChildren(path).empty()) return false;
    directories_.erase(path);
    return true;
  }

  bool rename(const char* oldPath, const char* newPath) {
    ++renameCalls_;
    renameHistory_.emplace_back(oldPath, newPath);
    if (failRenameCall_ != 0 && renameCalls_ == failRenameCall_) return false;
    if (exists(newPath) || directories_.count(parentPath(newPath)) == 0) return false;
    const auto source = files_.find(oldPath);
    if (source != files_.end()) {
      files_.emplace(newPath, std::move(source->second));
      files_.erase(source);
      return true;
    }
    if (directories_.count(oldPath) == 0) return false;

    const std::string oldPrefix = withTrailingSlash(oldPath);
    const std::string newPrefix = withTrailingSlash(newPath);
    std::vector<std::pair<std::string, std::vector<unsigned char>>> movedFiles;
    std::vector<std::string> movedDirectories;
    for (const auto& [path, content] : files_) {
      if (path.compare(0, oldPrefix.size(), oldPrefix) == 0) {
        movedFiles.emplace_back(newPrefix + path.substr(oldPrefix.size()), content);
      }
    }
    for (const std::string& path : directories_) {
      if (path == oldPath) {
        movedDirectories.push_back(newPath);
      } else if (path.compare(0, oldPrefix.size(), oldPrefix) == 0) {
        movedDirectories.push_back(newPrefix + path.substr(oldPrefix.size()));
      }
    }
    for (auto file = files_.begin(); file != files_.end();) {
      file = file->first.compare(0, oldPrefix.size(), oldPrefix) == 0 ? files_.erase(file) : std::next(file);
    }
    for (auto directory = directories_.begin(); directory != directories_.end();) {
      directory = (*directory == oldPath || directory->compare(0, oldPrefix.size(), oldPrefix) == 0)
                      ? directories_.erase(directory)
                      : std::next(directory);
    }
    for (auto& [path, content] : movedFiles) files_.emplace(std::move(path), std::move(content));
    directories_.insert(movedDirectories.begin(), movedDirectories.end());
    return true;
  }
  bool rename(const std::string& oldPath, const std::string& newPath) {
    return rename(oldPath.c_str(), newPath.c_str());
  }

  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    if (files_.count(path) == 0) return false;
    file = makeDataFile(path, false);
    return true;
  }

  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    if (directories_.count(parentPath(path)) == 0) return false;
    files_[path].clear();
    file = makeDataFile(path, true);
    return true;
  }

  void reset() {
    files_.clear();
    directories_.clear();
    renameCalls_ = 0;
    renameHistory_.clear();
    failRenameCall_ = 0;
    failDeletePath_.clear();
    failDeleteCount_ = 0;
    failRmdirOnce_ = false;
    shortWriteOnce_ = false;
    failSyncOnce_ = false;
    directories_.insert("/");
  }

  void addDirectory(const std::string& path) {
    if (path.empty() || path == "/") {
      directories_.insert("/");
      return;
    }
    addDirectory(parentPath(path));
    directories_.insert(path);
  }

  void setFile(const std::string& path, std::vector<unsigned char> bytes) {
    addDirectory(parentPath(path));
    files_[path] = std::move(bytes);
  }

  const std::vector<unsigned char>& file(const std::string& path) const { return files_.at(path); }
  std::vector<std::string> filesUnder(const std::string& path) const {
    std::vector<std::string> result;
    const std::string prefix = withTrailingSlash(path);
    for (const auto& [name, unused] : files_) {
      (void)unused;
      if (name.compare(0, prefix.size(), prefix) == 0) result.push_back(name);
    }
    return result;
  }

  void failRenameOnCall(const size_t call) { failRenameCall_ = call; }
  const std::vector<std::pair<std::string, std::string>>& renameHistory() const { return renameHistory_; }
  void failDeletePathOnce(std::string path) { failDeletePathTimes(std::move(path), 1); }
  void failDeletePathTimes(std::string path, const size_t count) {
    failDeletePath_ = std::move(path);
    failDeleteCount_ = count;
  }
  void failRmdirOnce() { failRmdirOnce_ = true; }
  void shortWriteOnce() { shortWriteOnce_ = true; }
  void failSyncOnce() { failSyncOnce_ = true; }

 private:
  friend class HalFile;
  std::map<std::string, std::vector<unsigned char>> files_;
  std::set<std::string> directories_;
  size_t renameCalls_ = 0;
  std::vector<std::pair<std::string, std::string>> renameHistory_;
  size_t failRenameCall_ = 0;
  std::string failDeletePath_;
  size_t failDeleteCount_ = 0;
  bool failRmdirOnce_ = false;
  bool shortWriteOnce_ = false;
  bool failSyncOnce_ = false;

  static std::string withTrailingSlash(const std::string& path) { return path.back() == '/' ? path : path + "/"; }

  static std::string parentPath(const std::string& path) {
    const size_t separator = path.rfind('/');
    if (separator == std::string::npos) return {};
    return separator == 0 ? "/" : path.substr(0, separator);
  }

  static std::string baseName(const std::string& path) {
    const size_t separator = path.rfind('/');
    return separator == std::string::npos ? path : path.substr(separator + 1);
  }

  bool shouldFailDelete(const std::string& path) {
    if (failDeletePath_ != path || failDeleteCount_ == 0) return false;
    --failDeleteCount_;
    if (failDeleteCount_ == 0) failDeletePath_.clear();
    return true;
  }

  std::vector<HalFile::Entry> immediateChildren(const std::string& path) const {
    std::map<std::string, bool> children;
    const std::string prefix = withTrailingSlash(path);
    for (const std::string& directory : directories_) {
      if (directory.compare(0, prefix.size(), prefix) != 0) continue;
      const std::string remainder = directory.substr(prefix.size());
      if (!remainder.empty() && remainder.find('/') == std::string::npos) children[remainder] = true;
    }
    for (const auto& [file, unused] : files_) {
      (void)unused;
      if (file.compare(0, prefix.size(), prefix) != 0) continue;
      const std::string remainder = file.substr(prefix.size());
      if (!remainder.empty() && remainder.find('/') == std::string::npos) children.emplace(remainder, false);
    }

    std::vector<HalFile::Entry> result;
    for (const auto& [name, directory] : children) result.push_back({name, directory});
    return result;
  }

  HalFile makeDataFile(const std::string& path, const bool writable) {
    HalFile file;
    file.open_ = true;
    file.storage_ = this;
    file.path_ = path;
    file.name_ = baseName(path);
    file.writable_ = writable;
    return file;
  }
};

inline size_t HalFile::getName(char* destination, const size_t capacity) const {
  if (!open_ || !destination || name_.empty() || name_.size() + 1 > capacity) return 0;
  memcpy(destination, name_.c_str(), name_.size() + 1);
  return name_.size();
}

inline HalFile HalFile::openNextFile() {
  HalFile result;
  if (!open_ || !directory_ || nextEntry_ >= entries_.size()) return result;
  const Entry& entry = entries_[nextEntry_++];
  result.open_ = true;
  result.directory_ = entry.directory;
  result.name_ = entry.name;
  return result;
}

inline uint64_t HalFile::fileSize64() const {
  return open_ && !directory_ && storage_ ? storage_->files_.at(path_).size() : 0;
}

inline int HalFile::read(void* destination, const size_t length) {
  if (!open_ || directory_ || !storage_ || position_ > fileSize64() || length > fileSize64() - position_) return 0;
  const auto& content = storage_->files_.at(path_);
  std::copy_n(content.data() + position_, length, static_cast<unsigned char*>(destination));
  position_ += length;
  return static_cast<int>(length);
}

inline size_t HalFile::write(const void* source, const size_t length) {
  if (!open_ || directory_ || !writable_ || !storage_) return 0;
  size_t written = length;
  if (storage_->shortWriteOnce_) {
    storage_->shortWriteOnce_ = false;
    written = length == 0 ? 0 : length - 1;
  }
  auto& content = storage_->files_[path_];
  if (position_ + written > content.size()) content.resize(position_ + written);
  std::copy_n(static_cast<const unsigned char*>(source), written, content.data() + position_);
  position_ += written;
  return written;
}

inline bool HalFile::sync() {
  if (!open_ || !storage_) return false;
  if (storage_->failSyncOnce_) {
    storage_->failSyncOnce_ = false;
    return false;
  }
  return true;
}

#define Storage HalStorage::getInstance()
