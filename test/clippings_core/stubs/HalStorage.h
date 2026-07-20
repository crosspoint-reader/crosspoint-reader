#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
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

  bool seek(size_t position);
  int read(void* data, size_t length);
  size_t write(const void* data, size_t length);
  void flush() {}
  size_t position() const { return position_; }
  size_t fileSize() const;
  uint64_t fileSize64() const { return fileSize(); }
  bool sync();
  bool close();
  explicit operator bool() const { return open_; }

 private:
  friend class HalStorage;
  HalStorage* storage_ = nullptr;
  std::string path_;
  size_t position_ = 0;
  bool writable_ = false;
  bool open_ = false;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool exists(const char* path) const { return files_.count(path) != 0 || directories_.count(path) != 0; }
  bool mkdir(const char* path, bool = true) {
    directories_.insert(path);
    return true;
  }
  bool remove(const char* path) {
    if (failNextRemove_) {
      failNextRemove_ = false;
      return false;
    }
    return files_.erase(path) != 0;
  }
  bool rename(const char* oldPath, const char* newPath) {
    ++renameCalls_;
    if (failRenameCalls_.erase(renameCalls_) != 0) return false;
    if (failNextRename_) {
      failNextRename_ = false;
      return false;
    }
    const auto found = files_.find(oldPath);
    if (found == files_.end() || files_.count(newPath) != 0) return false;
    files_.emplace(newPath, std::move(found->second));
    files_.erase(found);
    if (corruptNextRename_) {
      corruptNextRename_ = false;
      auto& bytes = files_.at(newPath);
      if (bytes.empty()) {
        bytes.push_back(0xFF);
      } else {
        bytes.back() ^= 0xFF;
      }
    }
    return true;
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    if (unreadablePaths_.count(path) != 0) return false;
    const auto found = files_.find(path);
    if (found == files_.end()) return false;
    file = makeFile(path, false);
    return true;
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    files_[path].clear();
    file = makeFile(path, true);
    return true;
  }

  void reset() {
    files_.clear();
    directories_.clear();
    unreadablePaths_.clear();
    shortWriteNext_ = false;
    failSyncNext_ = false;
    failNextRename_ = false;
    failNextRemove_ = false;
    corruptNextRename_ = false;
    renameCalls_ = 0;
    failRenameCalls_.clear();
  }
  void setFile(const std::string& path, std::vector<uint8_t> bytes) { files_[path] = std::move(bytes); }
  const std::vector<uint8_t>& file(const std::string& path) const { return files_.at(path); }
  void makeUnreadable(const std::string& path) { unreadablePaths_.insert(path); }
  void shortWriteOnce() { shortWriteNext_ = true; }
  void failSyncOnce() { failSyncNext_ = true; }
  void failRenameOnce() { failNextRename_ = true; }
  void failRenameOnCalls(std::initializer_list<size_t> relativeCalls) {
    for (const size_t call : relativeCalls) failRenameCalls_.insert(renameCalls_ + call);
  }
  void failRemoveOnce() { failNextRemove_ = true; }
  void corruptRenameOnce() { corruptNextRename_ = true; }

 private:
  friend class HalFile;
  std::map<std::string, std::vector<uint8_t>> files_;
  std::set<std::string> directories_;
  std::set<std::string> unreadablePaths_;
  bool shortWriteNext_ = false;
  bool failSyncNext_ = false;
  bool failNextRename_ = false;
  bool failNextRemove_ = false;
  bool corruptNextRename_ = false;
  size_t renameCalls_ = 0;
  std::set<size_t> failRenameCalls_;

  HalFile makeFile(const std::string& path, const bool writable) {
    HalFile file;
    file.storage_ = this;
    file.path_ = path;
    file.writable_ = writable;
    file.open_ = true;
    return file;
  }
};

inline bool HalFile::seek(const size_t position) {
  if (!open_ || position > fileSize()) return false;
  position_ = position;
  return true;
}

inline int HalFile::read(void* data, const size_t length) {
  if (!open_ || position_ > fileSize() || length > fileSize() - position_) return 0;
  const auto& bytes = storage_->files_.at(path_);
  std::copy_n(bytes.data() + position_, length, static_cast<uint8_t*>(data));
  position_ += length;
  return static_cast<int>(length);
}

inline size_t HalFile::write(const void* data, const size_t length) {
  if (!open_ || !writable_) return 0;
  size_t written = length;
  if (storage_->shortWriteNext_) {
    storage_->shortWriteNext_ = false;
    written = length == 0 ? 0 : length - 1;
  }
  auto& bytes = storage_->files_[path_];
  if (position_ + written > bytes.size()) bytes.resize(position_ + written);
  std::copy_n(static_cast<const uint8_t*>(data), written, bytes.data() + position_);
  position_ += written;
  return written;
}

inline size_t HalFile::fileSize() const {
  if (!open_) return 0;
  return storage_->files_.at(path_).size();
}

inline bool HalFile::sync() {
  if (!open_) return false;
  if (!storage_->failSyncNext_) return true;
  storage_->failSyncNext_ = false;
  return false;
}

inline bool HalFile::close() {
  const bool wasOpen = open_;
  open_ = false;
  return wasOpen;
}

#define Storage HalStorage::getInstance()
