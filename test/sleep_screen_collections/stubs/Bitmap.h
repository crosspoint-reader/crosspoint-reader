#pragma once

#include <cstdint>

#include "HalStorage.h"

enum class BmpReaderError : uint8_t {
  Ok,
  FileInvalid,
};

class Bitmap {
 public:
  explicit Bitmap(HalFile& file) : file(file) {}
  BmpReaderError parseHeaders() const {
    return file.hasValidBitmap() ? BmpReaderError::Ok : BmpReaderError::FileInvalid;
  }

 private:
  HalFile& file;
};
