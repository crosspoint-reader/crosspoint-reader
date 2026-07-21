#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using oflag_t = uint8_t;
inline constexpr oflag_t O_RDONLY = 0x01;
inline constexpr oflag_t O_RDWR = 0x02;
inline constexpr oflag_t O_CREAT = 0x10;
inline constexpr oflag_t O_TRUNC = 0x20;
inline constexpr oflag_t O_EXCL = 0x40;

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
  size_t getName(char* name, size_t length) const;
  uint8_t getError() const { return error_; }
  bool isDirectory() const { return open_ && directory_; }
  HalFile openNextFile();
  bool sync();
  bool close();
  explicit operator bool() const { return open_; }

 private:
  friend class HalStorage;
  HalStorage* storage_ = nullptr;
  std::string path_;
  size_t position_ = 0;
  bool writable_ = false;
  bool directory_ = false;
  bool open_ = false;
  uint8_t error_ = 0;
  std::vector<std::string> directoryEntries_;
  size_t nextDirectoryEntry_ = 0;
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
  HalFile open(const char* path, const oflag_t flags = O_RDONLY) {
    if ((flags & O_CREAT) != 0) {
      if (exclusiveCollisionPath_ == path) {
        files_[path] = std::move(exclusiveCollisionBytes_);
        exclusiveCollisionPath_.clear();
      }
      if (directories_.count(path) != 0 || ((flags & O_EXCL) != 0 && files_.count(path) != 0)) return {};
      if (files_.count(path) == 0 || (flags & O_TRUNC) != 0) files_[path].clear();
      return makeFile(path, true);
    }
    if (directories_.count(path) != 0) return makeFile(path, false, true);
    if (files_.count(path) != 0) return makeFile(path, false);
    return {};
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
    failDirectoryIterationAfter_ = std::numeric_limits<size_t>::max();
    corruptNextRename_ = false;
    corruptWritableCloseNext_ = false;
    exclusiveCollisionPath_.clear();
    exclusiveCollisionBytes_.clear();
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
  void failDirectoryIterationAfter(const size_t successfulEntries) { failDirectoryIterationAfter_ = successfulEntries; }
  void corruptRenameOnce() { corruptNextRename_ = true; }
  void corruptWriteOnCloseOnce() { corruptWritableCloseNext_ = true; }
  void collideExclusiveOpenOnce(std::string path, std::vector<uint8_t> bytes) {
    exclusiveCollisionPath_ = std::move(path);
    exclusiveCollisionBytes_ = std::move(bytes);
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
  size_t failDirectoryIterationAfter_ = std::numeric_limits<size_t>::max();
  bool corruptNextRename_ = false;
  bool corruptWritableCloseNext_ = false;
  std::string exclusiveCollisionPath_;
  std::vector<uint8_t> exclusiveCollisionBytes_;
  size_t renameCalls_ = 0;
  std::set<size_t> failRenameCalls_;

  HalFile makeFile(const std::string& path, const bool writable, const bool directory = false) {
    HalFile file;
    file.storage_ = this;
    file.path_ = path;
    file.writable_ = writable;
    file.directory_ = directory;
    file.open_ = true;
    if (directory) {
      const std::string prefix = path == "/" ? "/" : path + "/";
      std::set<std::string> entries;
      const auto collect = [&](const std::string& candidate) {
        if (candidate.compare(0, prefix.size(), prefix) != 0) return;
        const std::string remainder = candidate.substr(prefix.size());
        if (!remainder.empty() && remainder.find('/') == std::string::npos) entries.insert(candidate);
      };
      for (const auto& [candidate, bytes] : files_) {
        static_cast<void>(bytes);
        collect(candidate);
      }
      for (const std::string& candidate : directories_) collect(candidate);
      file.directoryEntries_.assign(entries.begin(), entries.end());
    }
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
  if (!open_ || directory_) return 0;
  return storage_->files_.at(path_).size();
}

inline size_t HalFile::getName(char* name, const size_t length) const {
  if (!open_ || !name || length == 0) return 0;
  const size_t separator = path_.find_last_of('/');
  const std::string leaf = separator == std::string::npos ? path_ : path_.substr(separator + 1);
  // Match SdFat: an undersized destination is cleared and reported as zero,
  // not as a successful truncated name.
  if (leaf.size() >= length) {
    name[0] = '\0';
    return 0;
  }
  const size_t copied = std::min(leaf.size(), length - 1);
  std::memcpy(name, leaf.data(), copied);
  name[copied] = '\0';
  return copied;
}

inline HalFile HalFile::openNextFile() {
  if (open_ && directory_ && nextDirectoryEntry_ == storage_->failDirectoryIterationAfter_) {
    storage_->failDirectoryIterationAfter_ = std::numeric_limits<size_t>::max();
    error_ = 1;
    return {};
  }
  if (!open_ || !directory_ || nextDirectoryEntry_ >= directoryEntries_.size()) return {};
  const std::string& entry = directoryEntries_[nextDirectoryEntry_++];
  return storage_->makeFile(entry, false, storage_->directories_.count(entry) != 0);
}

inline bool HalFile::sync() {
  if (!open_) return false;
  if (!storage_->failSyncNext_) return true;
  storage_->failSyncNext_ = false;
  return false;
}

inline bool HalFile::close() {
  const bool wasOpen = open_;
  if (wasOpen && writable_ && storage_->corruptWritableCloseNext_) {
    storage_->corruptWritableCloseNext_ = false;
    auto& bytes = storage_->files_.at(path_);
    if (bytes.empty()) {
      bytes.push_back(0xFF);
    } else {
      bytes.back() ^= 0xFF;
    }
  }
  open_ = false;
  return wasOpen;
}

#define Storage HalStorage::getInstance()
