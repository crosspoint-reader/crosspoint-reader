#include "ReadingStatsStorage.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace {
constexpr char LOG_TAG[] = "STATSIO";

bool verifyFile(const char* path, const uint8_t* expected, const size_t expectedSize) {
  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file) || file.fileSize() != expectedSize) return false;

  std::array<uint8_t, 32> buffer{};
  size_t offset = 0;
  while (offset < expectedSize) {
    const size_t chunk = std::min(buffer.size(), expectedSize - offset);
    if (file.read(buffer.data(), chunk) != static_cast<int>(chunk) ||
        memcmp(buffer.data(), expected + offset, chunk) != 0) {
      file.close();
      return false;
    }
    offset += chunk;
  }
  return file.close();
}
}  // namespace

namespace ReadingStatsStorage {

ReadOutcome read(const char* path, uint8_t* data, const size_t capacity) {
  ReadOutcome outcome;
  if (!Storage.exists(path)) return outcome;

  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) {
    outcome.result = ReadResult::IoError;
    return outcome;
  }
  outcome.size = file.fileSize();
  if (outcome.size > capacity) {
    const int first = file.read();
    if (first >= 0) outcome.firstByte = static_cast<uint8_t>(first);
    file.close();
    outcome.result = ReadResult::TooLarge;
    return outcome;
  }
  if (outcome.size == 0 || !data || file.read(data, outcome.size) != static_cast<int>(outcome.size)) {
    file.close();
    outcome.result = ReadResult::IoError;
    return outcome;
  }
  outcome.firstByte = data[0];
  outcome.result = file.close() ? ReadResult::Ok : ReadResult::IoError;
  return outcome;
}

bool writeAtomic(const char* path, const char* backupPath, const bool rotateExisting, const uint8_t* data,
                 const size_t size) {
  if (!path || !data || size == 0) return false;
  const std::string tmpPath = std::string(path) + ".tmp";
  if (Storage.exists(tmpPath.c_str()) && !Storage.remove(tmpPath.c_str())) {
    LOG_ERR(LOG_TAG, "Could not remove stale temp file: %s", tmpPath.c_str());
    return false;
  }

  HalFile file;
  if (!Storage.openFileForWrite(LOG_TAG, tmpPath.c_str(), file)) {
    LOG_ERR(LOG_TAG, "Could not open temp file: %s", tmpPath.c_str());
    return false;
  }
  const size_t written = file.write(data, size);
  if (written != size) {
    LOG_ERR(LOG_TAG, "Short write to %s: %u/%u", tmpPath.c_str(), static_cast<unsigned>(written),
            static_cast<unsigned>(size));
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  file.flush();
  const bool synced = file.sync();
  const bool closed = file.close();
  if (!synced || !closed || !verifyFile(tmpPath.c_str(), data, size)) {
    LOG_ERR(LOG_TAG, "Could not sync or verify temp file: %s", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }

  bool rotated = false;
  if (rotateExisting && backupPath && Storage.exists(path)) {
    if (Storage.exists(backupPath) && !Storage.remove(backupPath)) {
      LOG_ERR(LOG_TAG, "Could not remove old backup: %s", backupPath);
      Storage.remove(tmpPath.c_str());
      return false;
    }
    if (!Storage.rename(path, backupPath)) {
      LOG_ERR(LOG_TAG, "Could not rotate primary to backup: %s", path);
      Storage.remove(tmpPath.c_str());
      return false;
    }
    rotated = true;
  } else if (Storage.exists(path) && !Storage.remove(path)) {
    LOG_ERR(LOG_TAG, "Could not replace primary: %s", path);
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (!Storage.rename(tmpPath.c_str(), path) || !verifyFile(path, data, size)) {
    LOG_ERR(LOG_TAG, "Could not publish or verify primary: %s", path);
    if (Storage.exists(path)) Storage.remove(path);
    if (rotated && backupPath && Storage.exists(backupPath)) Storage.rename(backupPath, path);
    if (Storage.exists(tmpPath.c_str())) Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

}  // namespace ReadingStatsStorage
