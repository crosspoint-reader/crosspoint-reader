#pragma once

#include <HalStorage.h>

class WriterFile {
 public:
  static bool openForAppend(const char* path, HalFile& file);
};
