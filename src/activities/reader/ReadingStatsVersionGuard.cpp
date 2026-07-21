#include "ReadingStatsVersionGuard.h"

#include <HalStorage.h>

#include <cctype>
#include <cstring>

namespace {

bool hasStatsSuffix(const char* suffix) {
  return strcmp(suffix, ".bin") == 0 || strcmp(suffix, ".bin.bak") == 0 || strcmp(suffix, ".bin.tmp") == 0 ||
         strcmp(suffix, ".bin.bak.tmp") == 0;
}

bool isNewerVersionedName(const char* name, const char* prefix, const uint16_t currentVersion) {
  if (!name || !prefix) return false;
  const size_t prefixLength = strlen(prefix);
  if (strncmp(name, prefix, prefixLength) != 0) return false;

  const char* cursor = name + prefixLength;
  if (!std::isdigit(static_cast<unsigned char>(*cursor))) return false;
  uint32_t version = 0;
  do {
    const uint8_t digit = static_cast<uint8_t>(*cursor - '0');
    version = version > UINT16_MAX / 10U ? UINT16_MAX + 1U : version * 10U + digit;
    ++cursor;
  } while (std::isdigit(static_cast<unsigned char>(*cursor)));
  return hasStatsSuffix(cursor) && version > currentVersion;
}

}  // namespace

namespace ReadingStatsVersionGuard {

Result scan(const char* directoryPath, const char* fileNamePrefix, const uint16_t currentVersion) {
  if (!directoryPath || !fileNamePrefix || !Storage.exists(directoryPath)) return Result::NoNewerFile;

  HalFile directory = Storage.open(directoryPath);
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return Result::IoError;
  }

  char name[128]{};
  while (true) {
    HalFile file = directory.openNextFile();
    if (!file) {
      const bool failed = directory.getError() != 0;
      const bool closed = directory.close();
      return failed || !closed ? Result::IoError : Result::NoNewerFile;
    }

    const bool isDirectory = file.isDirectory();
    const size_t nameLength = file.getName(name, sizeof(name));
    const bool closed = file.close();
    if (nameLength == 0 || !closed) {
      directory.close();
      return Result::IoError;
    }
    if (!isDirectory && isNewerVersionedName(name, fileNamePrefix, currentVersion)) {
      directory.close();
      return Result::NewerFile;
    }
  }
}

}  // namespace ReadingStatsVersionGuard
