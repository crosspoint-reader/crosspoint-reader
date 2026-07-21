#include "BookReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <string>

#include "ReadingStatsCodec.h"
#include "ReadingStatsStorage.h"

namespace {
constexpr char LOG_TAG[] = "BSTATS";
constexpr char CURRENT_FILE_NAME[] = "stats_v5.bin";
constexpr char PREVIOUS_FILE_NAME[] = "stats_v4.bin";
constexpr char LEGACY_FILE_NAME[] = "stats.bin";
constexpr std::array<const char*, 5> REMOVABLE_FILE_NAMES = {CURRENT_FILE_NAME, "stats_v5.bin.tmp", "stats_v5.bin.bak",
                                                             PREVIOUS_FILE_NAME, LEGACY_FILE_NAME};

struct LoadOutcome {
  ReadingStatsDecodeResult result = ReadingStatsDecodeResult::Invalid;
  ReadingStatsStorage::ReadResult readResult = ReadingStatsStorage::ReadResult::Missing;
};

LoadOutcome loadPath(const std::string& path, BookReadingStats& stats) {
  ReadingStatsCodec::BookBytes data{};
  const ReadingStatsStorage::ReadOutcome read = ReadingStatsStorage::read(path.c_str(), data.data(), data.size());
  if (read.result == ReadingStatsStorage::ReadResult::Missing) return {};
  if (read.result == ReadingStatsStorage::ReadResult::TooLarge) {
    return {ReadingStatsDecodeResult::NewerFormat, read.result};
  }
  if (read.result != ReadingStatsStorage::ReadResult::Ok) {
    return {ReadingStatsDecodeResult::Invalid, read.result};
  }
  return {ReadingStatsCodec::decode(data.data(), read.size, stats), read.result};
}

bool removeIfPresent(const std::string& path) { return !Storage.exists(path.c_str()) || Storage.remove(path.c_str()); }

bool isProtected(const LoadOutcome& outcome) {
  return ReadingStatsStorage::isProtectedExistingFile(outcome.readResult,
                                                      outcome.result == ReadingStatsDecodeResult::NewerFormat);
}

BookReadingStats::LoadStatus protectedLoadStatus(const LoadOutcome& outcome) {
  return outcome.result == ReadingStatsDecodeResult::NewerFormat ? BookReadingStats::LoadStatus::NewerFormat
                                                                 : BookReadingStats::LoadStatus::IoError;
}
}  // namespace

BookReadingStats BookReadingStats::load(const std::string& cachePath, LoadStatus* status) {
  const auto finish = [status](const LoadStatus result) {
    if (status) *status = result;
  };
  BookReadingStats stats;
  const std::string currentPath = cachePath + "/" + CURRENT_FILE_NAME;
  const LoadOutcome primary = loadPath(currentPath, stats);
  if (primary.result == ReadingStatsDecodeResult::Ok) {
    finish(LoadStatus::Ok);
    return stats;
  }
  if (isProtected(primary)) {
    LOG_ERR(LOG_TAG, "Unreadable or newer per-book stats detected; leaving them untouched: %s", currentPath.c_str());
    finish(protectedLoadStatus(primary));
    return {};
  }

  const LoadOutcome backup = loadPath(currentPath + ".bak", stats);
  if (backup.result == ReadingStatsDecodeResult::Ok) {
    LOG_INF(LOG_TAG, "Recovered per-book stats from backup: %s", currentPath.c_str());
    finish(LoadStatus::RecoveredBackup);
    return stats;
  }
  if (isProtected(backup)) {
    LOG_ERR(LOG_TAG, "Unreadable or newer per-book stats backup detected; leaving it untouched: %s",
            currentPath.c_str());
    finish(protectedLoadStatus(backup));
    return {};
  }

  const LoadOutcome temporary = loadPath(currentPath + ".tmp", stats);
  if (temporary.result == ReadingStatsDecodeResult::Ok) {
    LOG_INF(LOG_TAG, "Recovered per-book stats from interrupted write: %s", currentPath.c_str());
    finish(LoadStatus::RecoveredTemp);
    return stats;
  }
  if (isProtected(temporary)) {
    LOG_ERR(LOG_TAG, "Unreadable or newer per-book stats temp file detected; leaving it untouched: %s",
            currentPath.c_str());
    finish(protectedLoadStatus(temporary));
    return {};
  }

  bool anyInvalid = primary.readResult != ReadingStatsStorage::ReadResult::Missing ||
                    backup.readResult != ReadingStatsStorage::ReadResult::Missing ||
                    temporary.readResult != ReadingStatsStorage::ReadResult::Missing;
  for (const char* fallback : {PREVIOUS_FILE_NAME, LEGACY_FILE_NAME}) {
    const LoadOutcome migrated = loadPath(cachePath + "/" + fallback, stats);
    if (migrated.result == ReadingStatsDecodeResult::Ok) {
      LOG_INF(LOG_TAG, "Loaded legacy per-book stats: %s", fallback);
      finish(LoadStatus::LoadedLegacy);
      return stats;
    }
    if (isProtected(migrated)) {
      LOG_ERR(LOG_TAG, "Unreadable or newer legacy per-book stats detected; leaving them untouched: %s", fallback);
      finish(protectedLoadStatus(migrated));
      return {};
    }
    anyInvalid = anyInvalid || migrated.readResult != ReadingStatsStorage::ReadResult::Missing;
  }
  finish(anyInvalid ? LoadStatus::Invalid : LoadStatus::Missing);
  return {};
}

bool BookReadingStats::save(const std::string& cachePath) const {
  const std::string path = cachePath + "/" + CURRENT_FILE_NAME;
  BookReadingStats existing;
  const LoadOutcome primary = loadPath(path, existing);
  if (isProtected(primary)) {
    LOG_ERR(LOG_TAG, "Refusing to overwrite unreadable or newer per-book stats: %s", path.c_str());
    return false;
  }

  // writeAtomic() may either remove an old backup during rotation or make it
  // unreachable behind a newly published primary. Protect it independently.
  BookReadingStats backupStats;
  const LoadOutcome backup = loadPath(path + ".bak", backupStats);
  if (isProtected(backup)) {
    LOG_ERR(LOG_TAG, "Refusing to overwrite or hide unreadable or newer per-book stats backup: %s", path.c_str());
    return false;
  }

  BookReadingStats temporaryStats;
  const LoadOutcome temporary = loadPath(path + ".tmp", temporaryStats);
  if (isProtected(temporary)) {
    LOG_ERR(LOG_TAG, "Refusing to remove unreadable or newer per-book stats temp file: %s", path.c_str());
    return false;
  }

  const ReadingStatsCodec::BookBytes data = ReadingStatsCodec::encode(*this);
  const std::string backupPath = path + ".bak";
  return ReadingStatsStorage::writeAtomic(path.c_str(), backupPath.c_str(),
                                          primary.result == ReadingStatsDecodeResult::Ok, data.data(), data.size());
}

bool BookReadingStats::remove(const std::string& cachePath) {
  // Inspect every file before deleting any of them. A reset must not partly
  // delete known-good state before discovering an unreadable, corrupt, or
  // newer legacy file that CrossVi cannot safely classify.
  if (!std::all_of(REMOVABLE_FILE_NAMES.begin(), REMOVABLE_FILE_NAMES.end(), [&](const char* name) {
        const std::string path = cachePath + "/" + name;
        BookReadingStats existing;
        const LoadOutcome outcome = loadPath(path, existing);
        if (outcome.readResult != ReadingStatsStorage::ReadResult::Missing &&
            outcome.result != ReadingStatsDecodeResult::Ok) {
          LOG_ERR(LOG_TAG, "Refusing to delete unrecognized per-book stats file: %s", path.c_str());
          return false;
        }
        return true;
      })) {
    return false;
  }

  bool ok = true;
  for (const char* name : REMOVABLE_FILE_NAMES) {
    if (!removeIfPresent(cachePath + "/" + name)) {
      LOG_ERR(LOG_TAG, "Could not remove per-book stats file: %s", name);
      ok = false;
    }
  }
  return ok;
}
