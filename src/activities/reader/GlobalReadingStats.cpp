#include "GlobalReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>
#include <esp_mac.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "ReadingStatsCodec.h"
#include "ReadingStatsCompletionTransaction.h"
#include "ReadingStatsStorage.h"

namespace {
constexpr char LOG_TAG[] = "GSTATS";
constexpr char GLOBAL_STATS_PATH[] = "/.crosspoint/global_stats.bin";
constexpr char GLOBAL_STATS_BACKUP_PATH[] = "/.crosspoint/global_stats.bin.bak";
constexpr char GLOBAL_STATS_TEMP_PATH[] = "/.crosspoint/global_stats.bin.tmp";
constexpr char SYNCED_STATS_DIR[] = "/.crosspoint/synced_stats";

struct LoadOutcome {
  ReadingStatsDecodeResult result = ReadingStatsDecodeResult::Invalid;
  ReadingStatsStorage::ReadResult readResult = ReadingStatsStorage::ReadResult::Missing;
};

LoadOutcome loadPath(const char* path, GlobalReadingStats& stats) {
  ReadingStatsCodec::GlobalBytes data{};
  const ReadingStatsStorage::ReadOutcome read = ReadingStatsStorage::read(path, data.data(), data.size());
  if (read.result == ReadingStatsStorage::ReadResult::TooLarge) {
    return {ReadingStatsDecodeResult::NewerFormat, read.result};
  }
  if (read.result != ReadingStatsStorage::ReadResult::Ok) {
    return {ReadingStatsDecodeResult::Invalid, read.result};
  }
  return {ReadingStatsCodec::decode(data.data(), read.size, stats), read.result};
}

ReadingStatsDecodeResult loadOpenFile(HalFile& file, GlobalReadingStats& stats) {
  const size_t size = file.fileSize();
  if (size > GlobalReadingStats::CURRENT_FILE_SIZE) return ReadingStatsDecodeResult::NewerFormat;
  if (size == 0) return ReadingStatsDecodeResult::Invalid;
  ReadingStatsCodec::GlobalBytes data{};
  if (file.read(data.data(), size) != static_cast<int>(size)) return ReadingStatsDecodeResult::Invalid;
  return ReadingStatsCodec::decode(data.data(), size, stats);
}

std::string localSyncedStatsFileName() {
  uint8_t mac[6]{};
  if (esp_efuse_mac_get_default(mac) != 0) return {};
  char name[32];
  snprintf(name, sizeof(name), "device_%02x%02x%02x%02x%02x%02x.bin", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return name;
}

bool isSyncedStatsFileName(const char* name) {
  if (!name || strlen(name) != 23 || strncmp(name, "device_", 7) != 0 || strcmp(name + 19, ".bin") != 0) {
    return false;
  }
  for (size_t i = 7; i < 19; ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(name[i]))) return false;
  }
  return true;
}

bool isSyncedStatsBackupFileName(const char* name) {
  return name && strlen(name) == 27 && strncmp(name, "device_", 7) == 0 && strcmp(name + 19, ".bin.bak") == 0 &&
         std::all_of(name + 7, name + 19,
                     [](const char character) { return std::isxdigit(static_cast<unsigned char>(character)); });
}

GlobalReadingStats::LoadStatus failedLoadStatus(const LoadOutcome& primary, const LoadOutcome& backup,
                                                const LoadOutcome& temporary) {
  if (primary.result == ReadingStatsDecodeResult::NewerFormat ||
      backup.result == ReadingStatsDecodeResult::NewerFormat ||
      temporary.result == ReadingStatsDecodeResult::NewerFormat) {
    return GlobalReadingStats::LoadStatus::NewerFormat;
  }
  if (primary.readResult == ReadingStatsStorage::ReadResult::IoError ||
      backup.readResult == ReadingStatsStorage::ReadResult::IoError ||
      temporary.readResult == ReadingStatsStorage::ReadResult::IoError) {
    return GlobalReadingStats::LoadStatus::IoError;
  }
  if (primary.readResult != ReadingStatsStorage::ReadResult::Missing ||
      backup.readResult != ReadingStatsStorage::ReadResult::Missing ||
      temporary.readResult != ReadingStatsStorage::ReadResult::Missing) {
    return GlobalReadingStats::LoadStatus::Invalid;
  }
  return GlobalReadingStats::LoadStatus::Missing;
}
}  // namespace

GlobalReadingStats GlobalReadingStats::load(LoadStatus* status) {
  const auto finish = [status](const LoadStatus result) {
    if (status) *status = result;
  };
  GlobalReadingStats stats;
  const LoadOutcome primary = loadPath(GLOBAL_STATS_PATH, stats);
  if (primary.result == ReadingStatsDecodeResult::Ok) {
    finish(LoadStatus::Ok);
    return stats;
  }
  if (primary.result == ReadingStatsDecodeResult::NewerFormat) {
    LOG_ERR(LOG_TAG, "Newer global stats detected; leaving them untouched");
    finish(LoadStatus::NewerFormat);
    return {};
  }
  if (primary.readResult == ReadingStatsStorage::ReadResult::IoError) {
    LOG_ERR(LOG_TAG, "Global stats are unreadable; refusing an older fallback");
    finish(LoadStatus::IoError);
    return {};
  }

  const LoadOutcome backup = loadPath(GLOBAL_STATS_BACKUP_PATH, stats);
  if (backup.result == ReadingStatsDecodeResult::Ok) {
    LOG_INF(LOG_TAG, "Recovered global stats from backup");
    finish(LoadStatus::RecoveredBackup);
    return stats;
  }
  if (backup.result == ReadingStatsDecodeResult::NewerFormat) {
    LOG_ERR(LOG_TAG, "Newer global stats backup detected; leaving it untouched");
    finish(LoadStatus::NewerFormat);
    return {};
  }
  if (backup.readResult == ReadingStatsStorage::ReadResult::IoError) {
    LOG_ERR(LOG_TAG, "Global stats backup is unreadable; refusing a temporary fallback");
    finish(LoadStatus::IoError);
    return {};
  }

  const LoadOutcome temporary = loadPath(GLOBAL_STATS_TEMP_PATH, stats);
  if (temporary.result == ReadingStatsDecodeResult::Ok) {
    LOG_INF(LOG_TAG, "Recovered global stats from interrupted write");
    finish(LoadStatus::RecoveredTemp);
    return stats;
  }
  if (temporary.result == ReadingStatsDecodeResult::NewerFormat) {
    LOG_ERR(LOG_TAG, "Newer global stats temp file detected; leaving it untouched");
  }
  finish(failedLoadStatus(primary, backup, temporary));
  return {};
}

bool GlobalReadingStats::hasSyncedStats() {
  HalFile directory = Storage.open(SYNCED_STATS_DIR);
  if (!directory) return false;
  const bool isDirectory = directory.isDirectory();
  directory.close();
  return isDirectory;
}

GlobalReadingStats GlobalReadingStats::loadAggregated() { return loadAggregated(load()); }

GlobalReadingStats GlobalReadingStats::loadAggregated(const GlobalReadingStats& localStats) {
  GlobalReadingStats aggregated = localStats;
  HalFile directory = Storage.open(SYNCED_STATS_DIR);
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return aggregated;
  }

  const std::string localFileName = localSyncedStatsFileName();
  char name[128]{};
  uint16_t loaded = 0;
  uint16_t skipped = 0;
  for (HalFile file = directory.openNextFile(); file; file = directory.openNextFile()) {
    const bool isDirectory = file.isDirectory();
    const size_t nameLength = file.getName(name, sizeof(name));
    if (isDirectory || nameLength == 0 || (!isSyncedStatsFileName(name) && !isSyncedStatsBackupFileName(name))) {
      file.close();
      continue;
    }

    const bool isBackup = isSyncedStatsBackupFileName(name);
    const std::string canonicalName(name, 23);
    if ((!localFileName.empty() && localFileName == canonicalName) ||
        (isBackup && Storage.exists((std::string(SYNCED_STATS_DIR) + "/" + canonicalName).c_str()))) {
      file.close();
      continue;
    }

    GlobalReadingStats peer;
    ReadingStatsDecodeResult decoded = loadOpenFile(file, peer);
    file.close();
    if (!isBackup && decoded != ReadingStatsDecodeResult::Ok) {
      const std::string backupPath = std::string(SYNCED_STATS_DIR) + "/" + canonicalName + ".bak";
      decoded = loadPath(backupPath.c_str(), peer).result;
    }
    if (decoded == ReadingStatsDecodeResult::Ok) {
      aggregated.merge(peer);
      ++loaded;
    } else {
      ++skipped;
    }
  }
  directory.close();
  if (loaded > 0 || skipped > 0) {
    LOG_INF(LOG_TAG, "Aggregated %u synced stats file(s), skipped %u", static_cast<unsigned>(loaded),
            static_cast<unsigned>(skipped));
  }
  return aggregated;
}

bool GlobalReadingStats::save() const {
  if (!ReadingStatsCompletionTransaction::permitsGlobalWrite(*this)) {
    LOG_ERR(LOG_TAG, "Refusing to overwrite a pending completion transaction");
    return false;
  }
  GlobalReadingStats existing;
  const LoadOutcome primary = loadPath(GLOBAL_STATS_PATH, existing);
  if (ReadingStatsStorage::isProtectedExistingFile(primary.readResult,
                                                   primary.result == ReadingStatsDecodeResult::NewerFormat)) {
    LOG_ERR(LOG_TAG, "Refusing to overwrite unreadable or newer global stats");
    return false;
  }

  const LoadOutcome backup = loadPath(GLOBAL_STATS_BACKUP_PATH, existing);
  if (ReadingStatsStorage::isProtectedExistingFile(backup.readResult,
                                                   backup.result == ReadingStatsDecodeResult::NewerFormat)) {
    LOG_ERR(LOG_TAG, "Refusing to overwrite or hide unreadable or newer global stats backup");
    return false;
  }

  const LoadOutcome temporary = loadPath(GLOBAL_STATS_TEMP_PATH, existing);
  if (ReadingStatsStorage::isProtectedExistingFile(temporary.readResult,
                                                   temporary.result == ReadingStatsDecodeResult::NewerFormat)) {
    LOG_ERR(LOG_TAG, "Refusing to remove unreadable or newer global stats temp file");
    return false;
  }
  const ReadingStatsCodec::GlobalBytes data = ReadingStatsCodec::encode(*this);
  return ReadingStatsStorage::writeAtomic(GLOBAL_STATS_PATH, GLOBAL_STATS_BACKUP_PATH,
                                          primary.result == ReadingStatsDecodeResult::Ok, data.data(), data.size());
}

bool GlobalReadingStats::resetLocal() { return GlobalReadingStats{}.save(); }
