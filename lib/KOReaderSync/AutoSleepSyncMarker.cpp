#include "AutoSleepSyncMarker.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <functional>

#include "KOReaderCredentialStore.h"

namespace {
std::string markerPath(const std::string& bookCachePath) { return bookCachePath + "/sleepsync.bin"; }
}  // namespace

std::string AutoSleepSyncMarker::bookCachePathFor(const std::string& epubPath) {
  return "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(epubPath));
}

uint32_t AutoSleepSyncMarker::serverFingerprint() {
  const std::string identity = KOREADER_STORE.getBaseUrl() + "\n" + KOREADER_STORE.getUsername() + "\n" +
                               std::to_string(static_cast<int>(KOREADER_STORE.getMatchMethod()));
  return static_cast<uint32_t>(std::hash<std::string>{}(identity));
}

bool AutoSleepSyncMarker::load(const std::string& bookCachePath, AutoSleepSyncMarkerData& out) {
  uint8_t buf[FILE_SIZE];
  {
    HalFile file;
    if (!Storage.openFileForRead("SlpMk", markerPath(bookCachePath), file)) return false;
    const int bytesRead = file.read(buf, sizeof(buf));
    if (bytesRead < 0 || static_cast<size_t>(bytesRead) != sizeof(buf)) return false;
  }
  if (buf[0] != FILE_VERSION) return false;
  // memcpy per field: RISC-V faults on unaligned multi-byte loads from a byte buffer.
  memcpy(&out.serverFingerprint, buf + 1, sizeof(out.serverFingerprint));
  memcpy(&out.spineIndex, buf + 5, sizeof(out.spineIndex));
  memcpy(&out.pageNumber, buf + 7, sizeof(out.pageNumber));
  memcpy(&out.totalPages, buf + 9, sizeof(out.totalPages));
  return true;
}

void AutoSleepSyncMarker::save(const std::string& bookCachePath, const AutoSleepSyncMarkerData& data) {
  // Value-change guard: repeated already-synced terminals at the same position
  // must not rewrite the file. Field-wise compare — memcmp would read padding.
  AutoSleepSyncMarkerData existing;
  if (load(bookCachePath, existing) && existing.serverFingerprint == data.serverFingerprint &&
      existing.spineIndex == data.spineIndex && existing.pageNumber == data.pageNumber &&
      existing.totalPages == data.totalPages) {
    return;
  }

  uint8_t buf[FILE_SIZE];
  buf[0] = FILE_VERSION;
  memcpy(buf + 1, &data.serverFingerprint, sizeof(data.serverFingerprint));
  memcpy(buf + 5, &data.spineIndex, sizeof(data.spineIndex));
  memcpy(buf + 7, &data.pageNumber, sizeof(data.pageNumber));
  memcpy(buf + 9, &data.totalPages, sizeof(data.totalPages));

  HalFile file;
  if (!Storage.openFileForWrite("SlpMk", markerPath(bookCachePath), file)) {
    LOG_ERR("SlpMk", "Failed to open sync marker for write");
    return;
  }
  if (file.write(buf, sizeof(buf)) != sizeof(buf)) {
    LOG_ERR("SlpMk", "Failed to write sync marker");
  }
}
