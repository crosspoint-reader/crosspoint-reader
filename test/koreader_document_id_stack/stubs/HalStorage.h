#pragma once

#include <string>

#include "Fixture.h"

class HalFile {
 public:
  ~HalFile() {
    if (opened) ++documentIdFixture::state.closedFiles;
  }

  size_t fileSize() const { return documentIdFixture::state.fileSize; }
  bool seekSet(const size_t offset) {
    auto& fixture = documentIdFixture::state;
    fixture.seeks.push_back(offset);
    cursor = offset;
    return true;
  }

  // Match the production HAL's signed read result.
  int read(void* destination, const size_t count) {
    auto& fixture = documentIdFixture::state;
    fixture.reads.emplace_back(cursor, count);
    auto* bytes = static_cast<uint8_t*>(destination);
    for (size_t i = 0; i < count; ++i) bytes[i] = documentIdFixture::patternByte(cursor + i);
    return static_cast<int>(count);
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
