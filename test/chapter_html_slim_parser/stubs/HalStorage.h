#pragma once

// Minimal host-test stand-in for lib/hal/HalStorage.h. HalFile wraps a plain
// std::fstream against real host paths (no SD emulation): tests write a
// fixture HTML file to a temp path and hand that path straight to
// Storage.openFileForRead, exactly like production code hands it a path on
// the SD card. Only the subset of the real API that ChapterHtmlSlimParser.cpp
// and CssParser.cpp actually call is implemented.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "Print.h"

class HalFile : public Print {
  friend class HalStorage;

 public:
  HalFile();
  ~HalFile() override;
  HalFile(HalFile&&) noexcept;
  HalFile& operator=(HalFile&&) noexcept;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  using Print::write;

  void flush();
  size_t size();
  bool isOpen() const;
  int available() const;
  size_t position() const;
  bool seek(size_t pos);
  int read(void* buf, size_t count);
  size_t write(const void* buf, size_t count);
  size_t write(uint8_t b) override;
  bool close();
  operator bool() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit HalFile(std::unique_ptr<Impl> impl);
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool exists(const char* path);
  bool remove(const char* path);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file);
};

#define Storage HalStorage::getInstance()
