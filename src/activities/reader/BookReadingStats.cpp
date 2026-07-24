#include "BookReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <string>

#include "ReadingStatsCodec.h"
#include "ReadingStatsCompletionTransaction.h"
#include "ReadingStatsEnvelope.h"
#include "ReadingStatsStorage.h"
#include "ReadingStatsVersionGuard.h"

namespace {
constexpr char LOG_TAG[] = "BSTATS";
constexpr char CURRENT_FILE_NAME[] = "stats_v6.bin";
constexpr uint16_t CURRENT_CANONICAL_VERSION = 6;
constexpr std::array<const char*, 5> LEGACY_FILE_NAMES = {"stats_v5.bin", "stats_v5.bin.bak", "stats_v5.bin.tmp",
                                                          "stats_v4.bin", "stats.bin"};

struct LoadOutcome {
  ReadingStatsDecodeResult result = ReadingStatsDecodeResult::Invalid;
  ReadingStatsStorage::ReadResult readResult = ReadingStatsStorage::ReadResult::Missing;
  bool wrongKind = false;
};

LoadOutcome loadEnvelopePath(const std::string& path, BookReadingStats& stats) {
  ReadingStatsCodec::BookBytes data{};
  const ReadingStatsEnvelope::ReadOutcome read =
      ReadingStatsEnvelope::read(path.c_str(), ReadingStatsEnvelope::Kind::Book, data.data(), data.size());
  if (read.readResult == ReadingStatsStorage::ReadResult::Missing) return {};
  if (read.readResult == ReadingStatsStorage::ReadResult::TooLarge) {
    return {ReadingStatsDecodeResult::NewerFormat, read.readResult, false};
  }
  if (read.readResult != ReadingStatsStorage::ReadResult::Ok) {
    return {ReadingStatsDecodeResult::Invalid, read.readResult, false};
  }
  if (read.decodeResult == ReadingStatsEnvelope::DecodeResult::NewerFormat ||
      read.decodeResult == ReadingStatsEnvelope::DecodeResult::PayloadTooLarge) {
    return {ReadingStatsDecodeResult::NewerFormat, read.readResult, false};
  }
  if (read.decodeResult != ReadingStatsEnvelope::DecodeResult::Ok) {
    return {ReadingStatsDecodeResult::Invalid, read.readResult,
            read.decodeResult == ReadingStatsEnvelope::DecodeResult::WrongKind};
  }
  return {ReadingStatsCodec::decode(data.data(), read.payloadSize, stats), read.readResult, false};
}

LoadOutcome loadLegacyPath(const std::string& path, BookReadingStats& stats) {
  ReadingStatsCodec::BookBytes data{};
  const ReadingStatsStorage::ReadOutcome read = ReadingStatsStorage::read(path.c_str(), data.data(), data.size());
  if (read.result == ReadingStatsStorage::ReadResult::Missing) return {};
  if (read.result == ReadingStatsStorage::ReadResult::TooLarge) {
    return {ReadingStatsDecodeResult::NewerFormat, read.result, false};
  }
  if (read.result != ReadingStatsStorage::ReadResult::Ok) {
    return {ReadingStatsDecodeResult::Invalid, read.result, false};
  }
  return {ReadingStatsCodec::decode(data.data(), read.size, stats), read.result, false};
}

bool isProtected(const LoadOutcome& outcome) {
  return ReadingStatsStorage::isProtectedExistingFile(outcome.readResult,
                                                      outcome.result == ReadingStatsDecodeResult::NewerFormat) ||
         outcome.wrongKind;
}

BookReadingStats::LoadStatus protectedLoadStatus(const LoadOutcome& outcome) {
  if (outcome.result == ReadingStatsDecodeResult::NewerFormat) return BookReadingStats::LoadStatus::NewerFormat;
  if (outcome.readResult == ReadingStatsStorage::ReadResult::IoError ||
      outcome.readResult == ReadingStatsStorage::ReadResult::TooLarge) {
    return BookReadingStats::LoadStatus::IoError;
  }
  return BookReadingStats::LoadStatus::Invalid;
}

bool legacyFilesAllowReset(const std::string& cachePath) {
  return std::all_of(LEGACY_FILE_NAMES.begin(), LEGACY_FILE_NAMES.end(), [&](const char* name) {
    const std::string path = cachePath + "/" + name;
    BookReadingStats ignored;
    const LoadOutcome outcome = loadLegacyPath(path, ignored);
    if (!isProtected(outcome)) return true;
    LOG_ERR(LOG_TAG, "Refusing to shadow newer or unreadable legacy per-book stats during reset: %s", path.c_str());
    return false;
  });
}

ReadingStatsVersionGuard::Result scanForNewerCanonicalFile(const std::string& cachePath) {
  return ReadingStatsVersionGuard::scan(cachePath.c_str(), "stats_v", CURRENT_CANONICAL_VERSION);
}

bool isExactPayload(const LoadOutcome& outcome, const BookReadingStats& stats,
                    const ReadingStatsCodec::BookBytes& expected) {
  return outcome.result == ReadingStatsDecodeResult::Ok && ReadingStatsCodec::encode(stats) == expected;
}

bool storageAllowsPublish(const std::string& cachePath) {
  if (scanForNewerCanonicalFile(cachePath) != ReadingStatsVersionGuard::Result::NoNewerFile) return false;
  const std::string path = cachePath + "/" + CURRENT_FILE_NAME;
  BookReadingStats ignored;
  return !isProtected(loadEnvelopePath(path, ignored)) && !isProtected(loadEnvelopePath(path + ".bak", ignored)) &&
         !isProtected(loadEnvelopePath(path + ".tmp", ignored));
}
}  // namespace

BookReadingStats BookReadingStats::load(const std::string& cachePath, LoadStatus* status) {
  const auto finish = [status](const LoadStatus result) {
    if (status) *status = result;
  };
  const ReadingStatsVersionGuard::Result versionGuard = scanForNewerCanonicalFile(cachePath);
  if (versionGuard != ReadingStatsVersionGuard::Result::NoNewerFile) {
    LOG_ERR(LOG_TAG, "A newer or unreadable per-book stats sibling exists; refusing the v6 view: %s",
            cachePath.c_str());
    finish(versionGuard == ReadingStatsVersionGuard::Result::NewerFile ? LoadStatus::NewerFormat : LoadStatus::IoError);
    return {};
  }
  BookReadingStats stats;
  const std::string currentPath = cachePath + "/" + CURRENT_FILE_NAME;
  const LoadOutcome primary = loadEnvelopePath(currentPath, stats);
  if (primary.result == ReadingStatsDecodeResult::Ok) {
    finish(LoadStatus::Ok);
    return stats;
  }
  if (isProtected(primary)) {
    LOG_ERR(LOG_TAG, "Unreadable or newer per-book stats detected; leaving them untouched: %s", currentPath.c_str());
    finish(protectedLoadStatus(primary));
    return {};
  }

  const LoadOutcome backup = loadEnvelopePath(currentPath + ".bak", stats);
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

  const LoadOutcome temporary = loadEnvelopePath(currentPath + ".tmp", stats);
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

  // A primary or backup is committed history and always shadows raw legacy,
  // even when corrupt. A lone invalid temp is only an abandoned write and may
  // be replaced by an idempotent legacy migration.
  const bool hasCommittedEnvelopeArtifact = primary.readResult != ReadingStatsStorage::ReadResult::Missing ||
                                            backup.readResult != ReadingStatsStorage::ReadResult::Missing;
  if (hasCommittedEnvelopeArtifact) {
    finish(LoadStatus::Invalid);
    return {};
  }

  bool anyInvalidLegacy = false;
  for (const char* legacyName : LEGACY_FILE_NAMES) {
    const std::string legacyPath = cachePath + "/" + legacyName;
    const LoadOutcome legacy = loadLegacyPath(legacyPath, stats);
    if (legacy.result == ReadingStatsDecodeResult::Ok) {
      // save() publishes the new envelope without modifying the source file.
      // A pending completion transaction may intentionally defer this copy;
      // the decoded payload remains safe to use and migration is idempotent.
      if (!stats.save(cachePath)) {
        LOG_ERR(LOG_TAG, "Loaded legacy per-book stats but could not publish the CrossVi envelope: %s",
                legacyPath.c_str());
      } else {
        LOG_INF(LOG_TAG, "Migrated legacy per-book stats without modifying the source: %s", legacyPath.c_str());
      }
      finish(LoadStatus::LoadedLegacy);
      return stats;
    }
    if (isProtected(legacy)) {
      LOG_ERR(LOG_TAG, "Unreadable or newer legacy per-book stats detected; leaving them untouched: %s",
              legacyPath.c_str());
      finish(protectedLoadStatus(legacy));
      return {};
    }
    anyInvalidLegacy = anyInvalidLegacy || legacy.readResult != ReadingStatsStorage::ReadResult::Missing;
  }
  finish(anyInvalidLegacy ? LoadStatus::Invalid : LoadStatus::Missing);
  return {};
}

bool BookReadingStats::canPublish(const std::string& cachePath) { return storageAllowsPublish(cachePath); }

bool BookReadingStats::save(const std::string& cachePath) const {
  if (!ReadingStatsCompletionTransaction::permitsBookWrite(cachePath, *this)) {
    LOG_ERR(LOG_TAG, "Refusing to overwrite a pending completion transaction: %s", cachePath.c_str());
    return false;
  }
  if (!storageAllowsPublish(cachePath)) {
    LOG_ERR(LOG_TAG, "Refusing to shadow protected per-book stats storage: %s", cachePath.c_str());
    return false;
  }
  const std::string path = cachePath + "/" + CURRENT_FILE_NAME;
  BookReadingStats existing;
  const LoadOutcome primary = loadEnvelopePath(path, existing);
  if (isProtected(primary)) {
    LOG_ERR(LOG_TAG, "Refusing to overwrite unreadable or newer per-book stats: %s", path.c_str());
    return false;
  }

  // writeAtomic() may either remove an old backup during rotation or make it
  // unreachable behind a newly published primary. Protect it independently.
  const ReadingStatsCodec::BookBytes data = ReadingStatsCodec::encode(*this);
  const std::string backupPath = path + ".bak";
  return ReadingStatsEnvelope::writeAtomic(path.c_str(), backupPath.c_str(),
                                           primary.result == ReadingStatsDecodeResult::Ok,
                                           ReadingStatsEnvelope::Kind::Book, data.data(), data.size());
}

bool BookReadingStats::saveRedundant(const std::string& cachePath) const {
  if (scanForNewerCanonicalFile(cachePath) != ReadingStatsVersionGuard::Result::NoNewerFile) return false;
  const std::string path = cachePath + "/" + CURRENT_FILE_NAME;
  const ReadingStatsCodec::BookBytes expected = ReadingStatsCodec::encode(*this);
  BookReadingStats primaryStats;
  LoadOutcome primary = loadEnvelopePath(path, primaryStats);
  if (isProtected(primary)) return false;
  BookReadingStats backupStats;
  LoadOutcome backup = loadEnvelopePath(path + ".bak", backupStats);
  if (isProtected(backup)) return false;
  if (isExactPayload(primary, primaryStats, expected) && isExactPayload(backup, backupStats, expected)) return true;

  if (!isExactPayload(primary, primaryStats, expected)) {
    if (!save(cachePath)) return false;
    primary = loadEnvelopePath(path, primaryStats);
    backup = loadEnvelopePath(path + ".bak", backupStats);
    if (isProtected(primary) || isProtected(backup)) return false;
    if (isExactPayload(primary, primaryStats, expected) && isExactPayload(backup, backupStats, expected)) return true;
  }

  // Rotating the verified expected primary once publishes the same payload as
  // the backup. A fully converged replay returns above without SD writes.
  if (!save(cachePath)) return false;
  primary = loadEnvelopePath(path, primaryStats);
  backup = loadEnvelopePath(path + ".bak", backupStats);
  return isExactPayload(primary, primaryStats, expected) && isExactPayload(backup, backupStats, expected);
}

bool BookReadingStats::remove(const std::string& cachePath) {
  if (!ReadingStatsCompletionTransaction::canRelocateOrDeleteBookCache(cachePath)) {
    LOG_ERR(LOG_TAG, "Refusing to reset stats with a pending completion transaction: %s", cachePath.c_str());
    return false;
  }
  if (scanForNewerCanonicalFile(cachePath) != ReadingStatsVersionGuard::Result::NoNewerFile) {
    LOG_ERR(LOG_TAG, "Refusing to reset beside a newer or unreadable per-book stats sibling: %s", cachePath.c_str());
    return false;
  }
  if (!legacyFilesAllowReset(cachePath)) return false;

  const std::string path = cachePath + "/" + CURRENT_FILE_NAME;
  BookReadingStats ignored;
  const LoadOutcome primary = loadEnvelopePath(path, ignored);
  const LoadOutcome backup = loadEnvelopePath(path + ".bak", ignored);
  const LoadOutcome temporary = loadEnvelopePath(path + ".tmp", ignored);
  if (isProtected(primary) || isProtected(backup) || isProtected(temporary)) {
    LOG_ERR(LOG_TAG, "Refusing to replace protected per-book stats during reset: %s", path.c_str());
    return false;
  }

  const BookReadingStats zero;
  const ReadingStatsCodec::BookBytes tombstone = ReadingStatsCodec::encode(zero);
  const std::string backupPath = path + ".bak";
  // Publish the backup first. Once this succeeds, loss of the old primary can
  // recover only zero; the retained CrossInk source can never be resurrected.
  if (!ReadingStatsEnvelope::writeAtomic(backupPath.c_str(), nullptr, false, ReadingStatsEnvelope::Kind::Book,
                                         tombstone.data(), tombstone.size()) ||
      !ReadingStatsEnvelope::writeAtomic(path.c_str(), nullptr, false, ReadingStatsEnvelope::Kind::Book,
                                         tombstone.data(), tombstone.size())) {
    return false;
  }
  BookReadingStats primaryStats;
  BookReadingStats backupStats;
  const LoadOutcome verifiedPrimary = loadEnvelopePath(path, primaryStats);
  const LoadOutcome verifiedBackup = loadEnvelopePath(backupPath, backupStats);
  return isExactPayload(verifiedPrimary, primaryStats, tombstone) &&
         isExactPayload(verifiedBackup, backupStats, tombstone);
}
