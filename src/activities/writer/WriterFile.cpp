#include "WriterFile.h"

// Keep append support local to the writer feature until it is needed more broadly.
bool WriterFile::openForAppend(const char* path, HalFile& file) {
  file = Storage.open(path, O_RDWR | O_CREAT | O_APPEND);
  return static_cast<bool>(file);
}
