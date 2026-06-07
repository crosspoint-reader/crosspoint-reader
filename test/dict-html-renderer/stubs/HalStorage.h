#pragma once
// Host-test stub for the project HAL (lib/hal/HalStorage.h), which is device-only
// (pulls in Arduino Print, SdFat, FreeRTOS). Backs HalFile with a real C FILE* so
// DictHtmlRenderer's renderFromFile / renderFromFileStreaming can be exercised
// against actual .dict files on the dev host. Only the subset the renderer uses
// is implemented.

#include <cstdint>
#include <cstdio>

class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool openForRead(const char* path) {
    close();
    fp_ = std::fopen(path, "rb");
    return fp_ != nullptr;
  }
  void seekSet(uint32_t offset) {
    if (fp_) std::fseek(fp_, static_cast<long>(offset), SEEK_SET);
  }
  int read(uint8_t* buf, int n) {
    if (!fp_ || n <= 0) return 0;
    return static_cast<int>(std::fread(buf, 1, static_cast<size_t>(n), fp_));
  }
  void close() {
    if (fp_) {
      std::fclose(fp_);
      fp_ = nullptr;
    }
  }

 private:
  std::FILE* fp_ = nullptr;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
  bool openFileForRead(const char* /*module*/, const char* path, HalFile& file) { return file.openForRead(path); }
};

#define Storage HalStorage::getInstance()
