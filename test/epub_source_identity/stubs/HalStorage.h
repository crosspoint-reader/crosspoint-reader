#pragma once

#include <Arduino.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(const uint8_t value) { return write(&value, 1); }
  virtual size_t write(const uint8_t*, size_t length) { return length; }
};

class HalStorage;

class HalFile : public Print {
 public:
  HalFile() = default;
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  size_t size() const;
  size_t fileSize() const { return size(); }
  uint64_t fileSize64() const { return size(); }
  bool isOpen() const { return open_; }
  bool seek(size_t position);
  bool seekSet(size_t position) { return seek(position); }
  bool seek64(uint64_t position) {
    return position <= static_cast<uint64_t>(SIZE_MAX) && seek(static_cast<size_t>(position));
  }
  bool seekCur(int64_t offset);
  int available() const;
  size_t position() const;
  uint8_t getError() const { return error_ ? 1 : 0; }
  int read(void* destination, size_t length);
  int read() {
    uint8_t value = 0;
    return read(&value, 1) == 1 ? value : -1;
  }
  using Print::write;
  size_t write(const uint8_t* source, size_t length) override;
  size_t write(const void* source, size_t length);
  void flush() {}
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
  bool error_ = false;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }

  bool exists(const char* path) const { return files_.count(path) != 0 || directories_.count(path) != 0; }
  bool mkdir(const char* path, bool = true) {
    if (exists(path)) return false;
    directories_.insert(path);
    return true;
  }
  bool remove(const char* path) { return files_.erase(path) != 0; }
  bool removeDir(const char* path) {
    if (failRemoveDir_) {
      failRemoveDir_ = false;
      return false;
    }
    const std::string root = path;
    const std::string prefix = root + "/";
    bool removed = directories_.erase(root) != 0;
    for (auto it = files_.begin(); it != files_.end();) {
      if (it->first.compare(0, prefix.size(), prefix) == 0) {
        it = files_.erase(it);
        removed = true;
      } else {
        ++it;
      }
    }
    for (auto it = directories_.begin(); it != directories_.end();) {
      if (it->compare(0, prefix.size(), prefix) == 0) {
        it = directories_.erase(it);
        removed = true;
      } else {
        ++it;
      }
    }
    return removed;
  }
  bool rename(const char* from, const char* to) {
    if (failRename_ || failRenameDestination_ == to) {
      failRename_ = false;
      failRenameDestination_.clear();
      return false;
    }
    const auto found = files_.find(from);
    if (found == files_.end() || files_.count(to) != 0) return false;
    files_[to] = found->second;
    files_.erase(found);
    if ((corruptRename_ || corruptRenameDestination_ == to) && !files_[to].empty()) {
      corruptRename_ = false;
      corruptRenameDestination_.clear();
      files_[to].back() ^= 0x80U;
    }
    return true;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    ++openReadAttempts_[path];
    if (unreadable_.count(path) || files_.count(path) == 0) return false;
    file = makeFile(path, false);
    return true;
  }
  bool openFileForRead(const char* tag, const std::string& path, HalFile& file) {
    return openFileForRead(tag, path.c_str(), file);
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    ++openWriteAttempts_[path];
    if (unwritable_.count(path)) return false;
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
    unreadable_.clear();
    unwritable_.clear();
    shortWrite_ = false;
    shortWritePath_.clear();
    shortReadPath_.clear();
    failSync_ = false;
    failClosePath_.clear();
    failRename_ = false;
    failRenameDestination_.clear();
    failRemoveDir_ = false;
    corruptRename_ = false;
    corruptRenameDestination_.clear();
    growOnReadCall_ = 0;
    readCalls_ = 0;
    maxRead_ = 0;
    invalidOperations_ = 0;
    openReadAttempts_.clear();
    openWriteAttempts_.clear();
    reportedSizes_.clear();
  }
  void setFile(const std::string& path, std::vector<uint8_t> data) { files_[path] = std::move(data); }
  std::vector<uint8_t>& mutableFile(const std::string& path) { return files_.at(path); }
  const std::vector<uint8_t>& file(const std::string& path) const { return files_.at(path); }
  void makeUnreadable(const std::string& path) { unreadable_.insert(path); }
  void makeReadable(const std::string& path) { unreadable_.erase(path); }
  void makeUnwritable(const std::string& path) { unwritable_.insert(path); }
  void makeWritable(const std::string& path) { unwritable_.erase(path); }
  void shortWriteOnce() { shortWrite_ = true; }
  void shortWriteFor(const std::string& path) { shortWritePath_ = path; }
  void shortReadFor(const std::string& path) { shortReadPath_ = path; }
  void failSyncOnce() { failSync_ = true; }
  void failCloseFor(const std::string& path) { failClosePath_ = path; }
  void failRenameOnce() { failRename_ = true; }
  void failRenameTo(const std::string& destination) { failRenameDestination_ = destination; }
  void failRemoveDirOnce() { failRemoveDir_ = true; }
  void corruptRenameOnce() { corruptRename_ = true; }
  void corruptRenameTo(const std::string& destination) { corruptRenameDestination_ = destination; }
  void growOnReadCall(size_t call) { growOnReadCall_ = call; }
  void reportFileSize(const std::string& path, const uint64_t size) { reportedSizes_[path] = size; }
  size_t maxRead() const { return maxRead_; }
  size_t invalidOperationCount() const { return invalidOperations_; }
  size_t openReadAttemptsFor(const std::string& path) const {
    const auto found = openReadAttempts_.find(path);
    return found == openReadAttempts_.end() ? 0 : found->second;
  }
  size_t openWriteAttemptsFor(const std::string& path) const {
    const auto found = openWriteAttempts_.find(path);
    return found == openWriteAttempts_.end() ? 0 : found->second;
  }

 private:
  friend class HalFile;
  std::map<std::string, std::vector<uint8_t>> files_;
  std::set<std::string> directories_;
  std::set<std::string> unreadable_;
  std::set<std::string> unwritable_;
  bool shortWrite_ = false;
  std::string shortWritePath_;
  std::string shortReadPath_;
  bool failSync_ = false;
  std::string failClosePath_;
  bool failRename_ = false;
  std::string failRenameDestination_;
  bool failRemoveDir_ = false;
  bool corruptRename_ = false;
  std::string corruptRenameDestination_;
  size_t growOnReadCall_ = 0;
  size_t readCalls_ = 0;
  size_t maxRead_ = 0;
  size_t invalidOperations_ = 0;
  std::map<std::string, size_t> openReadAttempts_;
  std::map<std::string, size_t> openWriteAttempts_;
  std::map<std::string, uint64_t> reportedSizes_;

  HalFile makeFile(const std::string& path, bool writable) {
    HalFile file;
    file.storage_ = this;
    file.path_ = path;
    file.writable_ = writable;
    file.open_ = true;
    return file;
  }
};

inline size_t HalFile::size() const {
  if (!open_ || !storage_) {
    ++HalStorage::getInstance().invalidOperations_;
    return 0;
  }
  const auto reported = storage_->reportedSizes_.find(path_);
  return reported == storage_->reportedSizes_.end() ? storage_->files_.at(path_).size()
                                                    : static_cast<size_t>(reported->second);
}

inline size_t HalFile::position() const {
  if (!open_ || !storage_) {
    ++HalStorage::getInstance().invalidOperations_;
    return 0;
  }
  return position_;
}

inline bool HalFile::seek(const size_t position) {
  if (!open_ || !storage_) {
    ++HalStorage::getInstance().invalidOperations_;
    return false;
  }
  if (position > size()) return false;
  position_ = position;
  return true;
}

inline bool HalFile::seekCur(const int64_t offset) {
  if (offset < 0 && static_cast<uint64_t>(-offset) > position_) return false;
  const uint64_t next = offset < 0 ? position_ - static_cast<uint64_t>(-offset) : position_ + offset;
  return next <= size() && seek(static_cast<size_t>(next));
}

inline int HalFile::available() const {
  if (!open_ || !storage_) {
    ++HalStorage::getInstance().invalidOperations_;
    return 0;
  }
  return position_ < size() ? static_cast<int>(size() - position_) : 0;
}

inline int HalFile::read(void* destination, const size_t length) {
  if (!open_ || !storage_) {
    ++HalStorage::getInstance().invalidOperations_;
    return 0;
  }
  if (writable_ || position_ >= size()) return 0;
  size_t readLength = std::min(length, size() - position_);
  if (storage_->shortReadPath_ == path_ && readLength > 0) {
    storage_->shortReadPath_.clear();
    --readLength;
    error_ = true;
  }
  storage_->maxRead_ = std::max(storage_->maxRead_, readLength);
  const auto& bytes = storage_->files_.at(path_);
  std::copy_n(bytes.data() + position_, readLength, static_cast<uint8_t*>(destination));
  position_ += readLength;
  ++storage_->readCalls_;
  if (storage_->growOnReadCall_ == storage_->readCalls_) storage_->files_[path_].push_back(0xA5U);
  return static_cast<int>(readLength);
}

inline size_t HalFile::write(const void* source, const size_t length) {
  if (!open_ || !storage_) {
    ++HalStorage::getInstance().invalidOperations_;
    return 0;
  }
  if (!writable_) return 0;
  size_t written = length;
  if (storage_->shortWrite_ || storage_->shortWritePath_ == path_) {
    storage_->shortWrite_ = false;
    storage_->shortWritePath_.clear();
    written = length == 0 ? 0 : length - 1;
  }
  auto& bytes = storage_->files_[path_];
  if (position_ + written > bytes.size()) bytes.resize(position_ + written);
  std::copy_n(static_cast<const uint8_t*>(source), written, bytes.data() + position_);
  position_ += written;
  return written;
}

inline size_t HalFile::write(const uint8_t* source, const size_t length) {
  return write(static_cast<const void*>(source), length);
}

inline bool HalFile::sync() {
  if (!open_ || !storage_) {
    ++HalStorage::getInstance().invalidOperations_;
    return false;
  }
  if (storage_->failSync_) {
    storage_->failSync_ = false;
    return false;
  }
  return true;
}

inline bool HalFile::close() {
  if (!open_ || !storage_) {
    ++HalStorage::getInstance().invalidOperations_;
    return false;
  }
  const bool shouldFail = storage_->failClosePath_ == path_;
  if (shouldFail) storage_->failClosePath_.clear();
  const bool wasOpen = open_;
  open_ = false;
  return wasOpen && !shouldFail;
}

#define Storage HalStorage::getInstance()
