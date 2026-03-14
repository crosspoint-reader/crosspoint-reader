#include "HalStorage.h"

#include <filesystem>

bool TestStorage::exists(const char* path) const { return std::filesystem::exists(path); }

bool TestStorage::remove(const char* path) const { return std::filesystem::remove(path); }

bool TestStorage::openFileForRead(const char*, const std::string& path, FsFile& file) const {
  file.close();
  file.stream_.open(path, std::ios::binary | std::ios::in);
  return file.stream_.is_open();
}

bool TestStorage::openFileForWrite(const char*, const std::string& path, FsFile& file) const {
  file.close();
  std::filesystem::path fsPath(path);
  if (fsPath.has_parent_path()) {
    std::filesystem::create_directories(fsPath.parent_path());
  }
  file.stream_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
  return file.stream_.is_open();
}
