#pragma once

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
  virtual size_t write(const uint8_t*, size_t length) { return length; }
};

class HalStorage;

class HalFile {
 public:
  HalFile() = default;
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  size_t size() const;
  size_t fileSize() const { return size(); }
  uint64_t fileSize64() const { return size(); }
  bool seek(size_t position);
  bool seekCur(int64_t offset);
  int available() const;
  size_t position() const { return position_; }
  int read(void* destination, size_t length);
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
  bool rename(const char* from, const char* to) {
    if (failRename_) {
      failRename_ = false;
      return false;
    }
    const auto found = files_.find(from);
    if (found == files_.end() || files_.count(to) != 0) return false;
    files_[to] = found->second;
    files_.erase(found);
    if (corruptRename_ && !files_[to].empty()) {
      corruptRename_ = false;
      files_[to].back() ^= 0x80U;
    }
    return true;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    if (unreadable_.count(path) || files_.count(path) == 0) return false;
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
    unreadable_.clear();
    shortWrite_ = false;
    failSync_ = false;
    failRename_ = false;
    corruptRename_ = false;
    growOnReadCall_ = 0;
    readCalls_ = 0;
    maxRead_ = 0;
  }
  void setFile(const std::string& path, std::vector<uint8_t> data) { files_[path] = std::move(data); }
  std::vector<uint8_t>& mutableFile(const std::string& path) { return files_.at(path); }
  const std::vector<uint8_t>& file(const std::string& path) const { return files_.at(path); }
  void makeUnreadable(const std::string& path) { unreadable_.insert(path); }
  void shortWriteOnce() { shortWrite_ = true; }
  void failSyncOnce() { failSync_ = true; }
  void failRenameOnce() { failRename_ = true; }
  void corruptRenameOnce() { corruptRename_ = true; }
  void growOnReadCall(size_t call) { growOnReadCall_ = call; }
  size_t maxRead() const { return maxRead_; }

 private:
  friend class HalFile;
  std::map<std::string, std::vector<uint8_t>> files_;
  std::set<std::string> directories_;
  std::set<std::string> unreadable_;
  bool shortWrite_ = false;
  bool failSync_ = false;
  bool failRename_ = false;
  bool corruptRename_ = false;
  size_t growOnReadCall_ = 0;
  size_t readCalls_ = 0;
  size_t maxRead_ = 0;

  HalFile makeFile(const std::string& path, bool writable) {
    HalFile file;
    file.storage_ = this;
    file.path_ = path;
    file.writable_ = writable;
    file.open_ = true;
    return file;
  }
};

inline size_t HalFile::size() const { return open_ && storage_ ? storage_->files_.at(path_).size() : 0; }

inline bool HalFile::seek(const size_t position) {
  if (!open_ || position > size()) return false;
  position_ = position;
  return true;
}

inline bool HalFile::seekCur(const int64_t offset) {
  if (offset < 0 && static_cast<uint64_t>(-offset) > position_) return false;
  const uint64_t next = offset < 0 ? position_ - static_cast<uint64_t>(-offset) : position_ + offset;
  return next <= size() && seek(static_cast<size_t>(next));
}

inline int HalFile::available() const { return open_ && position_ < size() ? static_cast<int>(size() - position_) : 0; }

inline int HalFile::read(void* destination, const size_t length) {
  if (!open_ || writable_ || position_ > size() || length > size() - position_) return 0;
  storage_->maxRead_ = std::max(storage_->maxRead_, length);
  const auto& bytes = storage_->files_.at(path_);
  std::copy_n(bytes.data() + position_, length, static_cast<uint8_t*>(destination));
  position_ += length;
  ++storage_->readCalls_;
  if (storage_->growOnReadCall_ == storage_->readCalls_) storage_->files_[path_].push_back(0xA5U);
  return static_cast<int>(length);
}

inline size_t HalFile::write(const void* source, const size_t length) {
  if (!open_ || !writable_) return 0;
  size_t written = length;
  if (storage_->shortWrite_) {
    storage_->shortWrite_ = false;
    written = length == 0 ? 0 : length - 1;
  }
  auto& bytes = storage_->files_[path_];
  if (position_ + written > bytes.size()) bytes.resize(position_ + written);
  std::copy_n(static_cast<const uint8_t*>(source), written, bytes.data() + position_);
  position_ += written;
  return written;
}

inline bool HalFile::sync() {
  if (!open_) return false;
  if (storage_->failSync_) {
    storage_->failSync_ = false;
    return false;
  }
  return true;
}

inline bool HalFile::close() {
  const bool wasOpen = open_;
  open_ = false;
  return wasOpen;
}

#define Storage HalStorage::getInstance()
