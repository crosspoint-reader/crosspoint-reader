#pragma once

#include <algorithm>
#include <cstring>
#include <string>

#include "Fixture.h"
#include "Logging.h"

class HalFile {
 public:
  ~HalFile() {
    if (opened) ++documentIdFixture::state.closedFiles;
  }

  size_t fileSize() const { return documentIdFixture::state.fileSize; }
  bool seekSet(const size_t offset) {
    auto& fixture = documentIdFixture::state;
    fixture.seeks.push_back(offset);
    if (fixture.failedSeek == offset) return false;
    cursor = offset;
    return true;
  }

  // Match the production HAL's signed read result, including negative errors.
  int read(void* destination, const size_t count) {
    auto& fixture = documentIdFixture::state;
    fixture.reads.emplace_back(cursor, count);
    const bool fault = fixture.faultReadOffset == cursor;
    const int result = fault ? fixture.faultReadResult : static_cast<int>(count);
    if (result > 0) {
      const size_t filled = std::min(count, static_cast<size_t>(result));
      auto* bytes = static_cast<uint8_t*>(destination);
      for (size_t i = 0; i < filled; ++i) bytes[i] = documentIdFixture::patternByte(cursor + i);
    }
    if (result < 0) LOG_ERR("KODoc", "Read failed at offset %zu", cursor);
    return result;
  }

  bool opened = false;

 private:
  size_t cursor = 0;
};

struct HalStorage {
  bool openFileForRead(const char*, const std::string&, HalFile& file) const {
    file.opened = documentIdFixture::state.openSucceeds;
    return file.opened;
  }
};

inline HalStorage Storage;
