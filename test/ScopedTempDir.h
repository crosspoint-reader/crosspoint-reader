#pragma once

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

class ScopedTempDir {
 public:
  ScopedTempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "crosspoint-test.XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) {
      throw std::system_error(errno, std::generic_category(), "mkdtemp");
    }
    path_ = pattern;
  }

  ~ScopedTempDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};
