#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

class HalFile {
 public:
  static inline int activeHandles = 0;
  HalFile() = default;
  explicit HalFile(std::filesystem::path path) : path_(std::move(path)), open_(std::filesystem::exists(path_)) {
    if (open_) activeHandles++;
    if (std::filesystem::is_directory(path_)) {
      for (const auto& entry : std::filesystem::directory_iterator(path_)) children_.push_back(entry.path());
    }
  }
  ~HalFile() { release(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  HalFile(HalFile&& other) noexcept { *this = std::move(other); }
  HalFile& operator=(HalFile&& other) noexcept {
    if (this != &other) {
      release();
      path_ = std::move(other.path_);
      children_ = std::move(other.children_);
      next_ = other.next_;
      open_ = std::exchange(other.open_, false);
    }
    return *this;
  }
  explicit operator bool() const { return open_; }
  bool isDirectory() const { return std::filesystem::is_directory(path_); }
  HalFile openNextFile() { return next_ < children_.size() ? HalFile(children_[next_++]) : HalFile(); }
  size_t getName(char* buffer, size_t len) {
    const std::string name = path_.filename().string();
    if (name.size() + 1 > len) return 0;
    name.copy(buffer, name.size());
    buffer[name.size()] = '\0';
    return name.size();
  }

 private:
  void release() {
    if (open_) activeHandles--;
    open_ = false;
  }
  std::filesystem::path path_;
  std::vector<std::filesystem::path> children_;
  size_t next_ = 0;
  bool open_ = false;
};

class HalStorage {
 public:
  std::filesystem::path card;
  bool exists(const char* path) const { return std::filesystem::exists(card / (path + 1)); }
  HalFile open(const char* path) const { return HalFile(card / (path + 1)); }
  bool mkdir(const char* path) const { return std::filesystem::create_directories(card / (path + 1)); }
  bool remove(const char* path) const { return std::filesystem::remove(card / (path + 1)); }
  bool rmdir(const char* path) const {
    std::error_code error;
    return std::filesystem::remove(card / (path + 1), error) && !error;
  }
  bool replaceFile(const char* stage, const char* target) const {
    const auto from = card / (stage + 1);
    const auto to = card / (target + 1);
    if (!std::filesystem::exists(from)) return false;
    std::error_code error;
    std::filesystem::remove(to, error);
    std::filesystem::rename(from, to, error);
    return !error;
  }
};

inline HalStorage Storage;
