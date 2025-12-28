#pragma once
#include <Print.h>

#include <string>

#include "miniz.h"

class ZipFile {
  std::string filePath;
  mutable mz_zip_archive zipArchive = {};
  bool loadFileStat(const char* filename, mz_zip_archive_file_stat* fileStat) const;
  long getDataOffset(const mz_zip_archive_file_stat& fileStat) const;

 public:
  explicit ZipFile(std::string filePath);
  ~ZipFile() { mz_zip_reader_end(&zipArchive); }
  bool getInflatedFileSize(const char* filename, size_t* size) const;
  uint8_t* readFileToMemory(const char* filename, size_t* size = nullptr, bool trailingNullByte = false) const;
  bool readFileToStream(const char* filename, Print& out, size_t chunkSize) const;
};
