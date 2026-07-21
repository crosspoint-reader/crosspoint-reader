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

  int read();
  int read(void* data, size_t length);
  size_t write(const void* data, size_t length);
  void flush() {}
  size_t fileSize() const;
  bool sync();
  bool close();
  bool isDirectory() const { return open_ && directory_; }
  size_t getName(char* name, size_t length) const;
  HalFile openNextFile();
  uint8_t getError() const { return error_; }
  explicit operator bool() const { return open_; }

 private:
  friend class HalStorage;
  HalStorage* storage_ = nullptr;
  std::string path_;
  size_t position_ = 0;
  bool writable_ = false;
  bool open_ = false;
  bool directory_ = false;
  size_t nextEntry_ = 0;
  std::vector<std::string> entries_;
  mutable uint8_t error_ = 0;
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
    ++removeCalls_;
    if (failNextRemove_ || (failRemoveCall_ != 0 && removeCalls_ == failRemoveCall_)) {
      failNextRemove_ = false;
      return false;
    }
    return files_.erase(path) != 0;
  }
  bool rename(const char* oldPath, const char* newPath) {
    ++renameCalls_;
    if (failNextRename_ || (failRenameCall_ != 0 && renameCalls_ == failRenameCall_)) {
      failNextRename_ = false;
      return false;
    }
    const auto found = files_.find(oldPath);
    if (found == files_.end() || files_.count(newPath) != 0) return false;
    files_.emplace(newPath, std::move(found->second));
    files_.erase(found);
    if (corruptNextRename_ || (corruptRenameCall_ != 0 && renameCalls_ == corruptRenameCall_)) {
      corruptNextRename_ = false;
      auto& bytes = files_.at(newPath);
      if (!bytes.empty()) bytes.back() ^= 0xFF;
    }
    return true;
  }
  HalFile open(const char* path) {
    if (directories_.count(path) == 0) {
      const auto found = files_.find(path);
      return found == files_.end() ? HalFile{} : makeFile(path, false);
    }
    HalFile directory = makeFile(path, false);
    directory.directory_ = true;
    const std::string prefix = std::string(path) + "/";
    for (const auto& [filePath, unused] : files_) {
      (void)unused;
      if (filePath.starts_with(prefix) && filePath.find('/', prefix.size()) == std::string::npos) {
        directory.entries_.push_back(filePath);
      }
    }
    return directory;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    if (unreadablePaths_.count(path) != 0 || files_.count(path) == 0) return false;
    file = makeFile(path, false);
    return true;
  }
  bool openFileForRead(const char* tag, const std::string& path, HalFile& file) {
    return openFileForRead(tag, path.c_str(), file);
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    files_[path].clear();
    file = makeFile(path, true);
    return true;
  }
  bool openFileForWrite(const char* tag, const std::string& path, HalFile& file) {
    return openFileForWrite(tag, path.c_str(), file);
  }

  void reset() {
    files_.clear();
    directories_.clear();
    unreadablePaths_.clear();
    resetFaultInjection();
  }
  void setFile(const std::string& path, std::vector<uint8_t> bytes) { files_[path] = std::move(bytes); }
  const std::vector<uint8_t>& file(const std::string& path) const { return files_.at(path); }
  void makeUnreadable(const std::string& path) { unreadablePaths_.insert(path); }
  void shortWriteOnce() { shortWriteNext_ = true; }
  void failSyncOnce() { failSyncNext_ = true; }
  void failRenameOnce() { failNextRename_ = true; }
  void failRemoveOnce() { failNextRemove_ = true; }
  void corruptRenameOnce() { corruptNextRename_ = true; }
  void corruptRenameOnCall(const size_t call) { corruptRenameCall_ = call; }
  void shortWriteOnCall(const size_t call) { shortWriteCall_ = call; }
  void failSyncOnCall(const size_t call) { failSyncCall_ = call; }
  void failRenameOnCall(const size_t call) { failRenameCall_ = call; }
  void failRemoveOnCall(const size_t call) { failRemoveCall_ = call; }
  void failDirectoryIterationAfter(const size_t entryCount) { failDirectoryIterationAfter_ = entryCount; }
  void failGetNameFor(std::string path) { failGetNamePaths_.insert(std::move(path)); }
  void failCloseFor(std::string path) { failClosePaths_.insert(std::move(path)); }
  size_t writeCallCount() const { return writeCalls_; }
  size_t renameCallCount() const { return renameCalls_; }
  void resetFaultInjection() {
    shortWriteNext_ = false;
    failSyncNext_ = false;
    failNextRename_ = false;
    failNextRemove_ = false;
    corruptNextRename_ = false;
    shortWriteCall_ = 0;
    failSyncCall_ = 0;
    failRenameCall_ = 0;
    failRemoveCall_ = 0;
    corruptRenameCall_ = 0;
    writeCalls_ = 0;
    syncCalls_ = 0;
    renameCalls_ = 0;
    removeCalls_ = 0;
    failDirectoryIterationAfter_ = SIZE_MAX;
    failGetNamePaths_.clear();
    failClosePaths_.clear();
  }

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
  size_t shortWriteCall_ = 0;
  size_t failSyncCall_ = 0;
  size_t failRenameCall_ = 0;
  size_t failRemoveCall_ = 0;
  size_t corruptRenameCall_ = 0;
  size_t writeCalls_ = 0;
  size_t syncCalls_ = 0;
  size_t renameCalls_ = 0;
  size_t removeCalls_ = 0;
  size_t failDirectoryIterationAfter_ = SIZE_MAX;
  std::set<std::string> failGetNamePaths_;
  std::set<std::string> failClosePaths_;

  HalFile makeFile(const std::string& path, const bool writable) {
    HalFile file;
    file.storage_ = this;
    file.path_ = path;
    file.writable_ = writable;
    file.open_ = true;
    return file;
  }
};

inline int HalFile::read() {
  uint8_t byte = 0;
  return read(&byte, 1) == 1 ? byte : -1;
}

inline int HalFile::read(void* data, const size_t length) {
  if (!open_ || directory_ || position_ > fileSize() || length > fileSize() - position_) return 0;
  const auto& bytes = storage_->files_.at(path_);
  std::copy_n(bytes.data() + position_, length, static_cast<uint8_t*>(data));
  position_ += length;
  return static_cast<int>(length);
}

inline size_t HalFile::write(const void* data, const size_t length) {
  if (!open_ || !writable_ || directory_) return 0;
  ++storage_->writeCalls_;
  size_t written = length;
  if (storage_->shortWriteNext_ ||
      (storage_->shortWriteCall_ != 0 && storage_->writeCalls_ == storage_->shortWriteCall_)) {
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
  if (!open_ || directory_) return 0;
  return storage_->files_.at(path_).size();
}

inline bool HalFile::sync() {
  if (!open_) return false;
  ++storage_->syncCalls_;
  if (storage_->failSyncNext_ || (storage_->failSyncCall_ != 0 && storage_->syncCalls_ == storage_->failSyncCall_)) {
    storage_->failSyncNext_ = false;
    return false;
  }
  return true;
}

inline bool HalFile::close() {
  const bool wasOpen = open_;
  open_ = false;
  const bool failed = wasOpen && storage_->failClosePaths_.erase(path_) != 0;
  return wasOpen && !failed;
}

inline size_t HalFile::getName(char* name, const size_t length) const {
  if (!open_ || !name || length == 0) return 0;
  if (storage_->failGetNamePaths_.count(path_) != 0) {
    error_ = 1;
    return 0;
  }
  const size_t slash = path_.find_last_of('/');
  const std::string base = path_.substr(slash == std::string::npos ? 0 : slash + 1);
  const size_t copied = std::min(base.size(), length - 1);
  memcpy(name, base.data(), copied);
  name[copied] = '\0';
  return copied;
}

inline HalFile HalFile::openNextFile() {
  if (!open_ || !directory_ || nextEntry_ >= entries_.size()) return {};
  if (nextEntry_ >= storage_->failDirectoryIterationAfter_) {
    error_ = 1;
    return {};
  }
  return storage_->makeFile(entries_[nextEntry_++], false);
}

#define Storage HalStorage::getInstance()
