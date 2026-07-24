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
#include "ReadingStatsEnvelope.h"
#include "ReadingStatsStorage.h"
#include "ReadingStatsVersionGuard.h"

namespace {
constexpr char LOG_TAG[] = "GSTATS";
constexpr char GLOBAL_STATS_PATH[] = "/.crosspoint/global_stats_v4.bin";
constexpr char GLOBAL_STATS_BACKUP_PATH[] = "/.crosspoint/global_stats_v4.bin.bak";
constexpr char GLOBAL_STATS_TEMP_PATH[] = "/.crosspoint/global_stats_v4.bin.tmp";
constexpr char GLOBAL_STATS_DIRECTORY[] = "/.crosspoint";
constexpr uint16_t CURRENT_GLOBAL_CANONICAL_VERSION = 4;
constexpr char LEGACY_GLOBAL_STATS_PATH[] = "/.crosspoint/global_stats.bin";
constexpr char SYNCED_STATS_DIR[] = "/.crosspoint/synced_stats";
constexpr char USER_BACKUP_DIRECTORY[] = "/.crosspoint/stats_backups";
constexpr char USER_BACKUP_PATH[] = "/.crosspoint/stats_backups/device_stats_v1.bin";
constexpr char USER_BACKUP_PREVIOUS_PATH[] = "/.crosspoint/stats_backups/device_stats_v1.bin.bak";
constexpr char USER_BACKUP_TEMP_PATH[] = "/.crosspoint/stats_backups/device_stats_v1.bin.tmp";
constexpr char DAILY_STATS_PATH[] = "/.crosspoint/daily_stats_v1.bin";
constexpr char DAILY_STATS_BACKUP_PATH[] = "/.crosspoint/daily_stats_v1.bin.bak";
constexpr char DAILY_STATS_TEMP_PATH[] = "/.crosspoint/daily_stats_v1.bin.tmp";
constexpr uint8_t DAILY_STATS_VERSION = 2;
constexpr size_t DAILY_STATS_PAYLOAD_SIZE = 17;

uint32_t readLe32(const uint8_t* data, const size_t offset) {
  return static_cast<uint32_t>(data[offset]) | static_cast<uint32_t>(data[offset + 1]) << 8 |
         static_cast<uint32_t>(data[offset + 2]) << 16 | static_cast<uint32_t>(data[offset + 3]) << 24;
}

void writeLe32(uint8_t* data, const size_t offset, const uint32_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
  data[offset + 2] = static_cast<uint8_t>(value >> 16);
  data[offset + 3] = static_cast<uint8_t>(value >> 24);
}

struct LoadOutcome {
  ReadingStatsDecodeResult result = ReadingStatsDecodeResult::Invalid;
  ReadingStatsStorage::ReadResult readResult = ReadingStatsStorage::ReadResult::Missing;
  bool wrongKind = false;
};

enum class DailyLoadStatus : uint8_t { Missing, Valid, Invalid, Protected };

struct DailyLoadOutcome {
  DailyLoadStatus status = DailyLoadStatus::Missing;
  uint32_t day = 0;
  uint32_t seconds = 0;
  uint32_t sessions = 0;
  uint32_t globalFingerprint = 0;
};

uint32_t globalFingerprint(const GlobalReadingStats& stats) {
  ReadingStatsCodec::GlobalBytes encoded = ReadingStatsCodec::encode(stats);
  // Page and completion counters do not change the daily summary. Excluding
  // them keeps the sidecar valid when a completion transaction republishes
  // the compatible global payload. totalSessions remains part of the
  // fingerprint because the sidecar now contains the exact daily count.
  std::fill(encoded.begin() + 9, encoded.begin() + 17, 0);  // totalPagesTurned + completedBooks
  return ReadingStatsEnvelope::crc32(encoded.data(), encoded.size());
}

DailyLoadOutcome loadDailyPath(const char* path) {
  std::array<uint8_t, DAILY_STATS_PAYLOAD_SIZE> payload{};
  const ReadingStatsEnvelope::ReadOutcome read =
      ReadingStatsEnvelope::read(path, ReadingStatsEnvelope::Kind::DailyGlobal, payload.data(), payload.size());
  if (read.readResult == ReadingStatsStorage::ReadResult::Missing) return {};
  if (read.readResult == ReadingStatsStorage::ReadResult::TooLarge ||
      read.readResult == ReadingStatsStorage::ReadResult::IoError ||
      read.decodeResult == ReadingStatsEnvelope::DecodeResult::NewerFormat ||
      read.decodeResult == ReadingStatsEnvelope::DecodeResult::PayloadTooLarge ||
      read.decodeResult == ReadingStatsEnvelope::DecodeResult::WrongKind) {
    return {DailyLoadStatus::Protected};
  }
  if (read.decodeResult != ReadingStatsEnvelope::DecodeResult::Ok || read.payloadSize != payload.size()) {
    return {DailyLoadStatus::Invalid};
  }
  if (payload[0] > DAILY_STATS_VERSION) return {DailyLoadStatus::Protected};
  if (payload[0] != DAILY_STATS_VERSION) return {DailyLoadStatus::Invalid};

  const uint32_t day = readLe32(payload.data(), 1);
  const uint32_t seconds = readLe32(payload.data(), 5);
  const uint32_t sessions = readLe32(payload.data(), 9);
  ReadingStatsDate date;
  if (!readingStatsDateFromDayIndex(day, date) || seconds == 0 || seconds > 24u * 3600u) {
    return {DailyLoadStatus::Invalid};
  }
  return {DailyLoadStatus::Valid, day, seconds, sessions, readLe32(payload.data(), 13)};
}

void loadDailySummary(GlobalReadingStats& stats) {
  const uint32_t fingerprint = globalFingerprint(stats);
  for (const char* path : {DAILY_STATS_PATH, DAILY_STATS_BACKUP_PATH, DAILY_STATS_TEMP_PATH}) {
    const DailyLoadOutcome loaded = loadDailyPath(path);
    if (loaded.status == DailyLoadStatus::Protected) return;
    if (loaded.status == DailyLoadStatus::Valid && loaded.globalFingerprint == fingerprint &&
        loaded.sessions <= stats.totalSessions) {
      stats.latestReadingDay = loaded.day;
      stats.latestDayReadingSeconds = loaded.seconds;
      stats.latestDaySessions = loaded.sessions;
      stats.hasLatestDayReadingSeconds = true;
      return;
    }
  }
}

bool saveDailySummary(const GlobalReadingStats& stats) {
  if (!stats.hasLatestDayReadingSeconds) return true;
  ReadingStatsDate date;
  if (!readingStatsDateFromDayIndex(stats.latestReadingDay, date) || stats.latestDayReadingSeconds == 0 ||
      stats.latestDayReadingSeconds > 24u * 3600u || stats.latestDaySessions > stats.totalSessions) {
    return false;
  }

  const DailyLoadOutcome primary = loadDailyPath(DAILY_STATS_PATH);
  const DailyLoadOutcome backup = loadDailyPath(DAILY_STATS_BACKUP_PATH);
  const DailyLoadOutcome temporary = loadDailyPath(DAILY_STATS_TEMP_PATH);
  if (primary.status == DailyLoadStatus::Protected || backup.status == DailyLoadStatus::Protected ||
      temporary.status == DailyLoadStatus::Protected) {
    LOG_ERR(LOG_TAG, "Refusing to overwrite protected daily reading statistics");
    return false;
  }

  std::array<uint8_t, DAILY_STATS_PAYLOAD_SIZE> payload{};
  payload[0] = DAILY_STATS_VERSION;
  writeLe32(payload.data(), 1, stats.latestReadingDay);
  writeLe32(payload.data(), 5, stats.latestDayReadingSeconds);
  writeLe32(payload.data(), 9, stats.latestDaySessions);
  writeLe32(payload.data(), 13, globalFingerprint(stats));
  return ReadingStatsEnvelope::writeAtomic(DAILY_STATS_PATH, DAILY_STATS_BACKUP_PATH,
                                           primary.status == DailyLoadStatus::Valid,
                                           ReadingStatsEnvelope::Kind::DailyGlobal, payload.data(), payload.size());
}

LoadOutcome loadEnvelopePath(const char* path, const ReadingStatsEnvelope::Kind kind, GlobalReadingStats& stats) {
  ReadingStatsCodec::GlobalBytes data{};
  const ReadingStatsEnvelope::ReadOutcome read = ReadingStatsEnvelope::read(path, kind, data.data(), data.size());
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

LoadOutcome loadLegacyPath(const char* path, GlobalReadingStats& stats) {
  ReadingStatsCodec::GlobalBytes data{};
  const ReadingStatsStorage::ReadOutcome read = ReadingStatsStorage::read(path, data.data(), data.size());
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

LoadOutcome loadPreferredEnvelope(const std::string& path, const ReadingStatsEnvelope::Kind kind,
                                  GlobalReadingStats& stats) {
  LoadOutcome failure;
  const std::array<std::string, 3> candidates = {path, path + ".bak", path + ".tmp"};
  for (const std::string& candidate : candidates) {
    const LoadOutcome outcome = loadEnvelopePath(candidate.c_str(), kind, stats);
    if (outcome.result == ReadingStatsDecodeResult::Ok || isProtected(outcome)) return outcome;
    if (outcome.readResult != ReadingStatsStorage::ReadResult::Missing) failure = outcome;
  }
  return failure;
}

LoadOutcome loadPreferredLegacy(const std::string& path, GlobalReadingStats& stats) {
  LoadOutcome failure;
  const std::array<std::string, 3> candidates = {path, path + ".bak", path + ".tmp"};
  for (const std::string& candidate : candidates) {
    const LoadOutcome outcome = loadLegacyPath(candidate.c_str(), stats);
    if (outcome.result == ReadingStatsDecodeResult::Ok || isProtected(outcome)) return outcome;
    if (outcome.readResult != ReadingStatsStorage::ReadResult::Missing) failure = outcome;
  }
  return failure;
}

bool localSyncedStatsIdentity(uint64_t& identity) {
  uint8_t mac[6]{};
  if (esp_efuse_mac_get_default(mac) != 0) return false;
  identity = static_cast<uint64_t>(mac[0]) << 40 | static_cast<uint64_t>(mac[1]) << 32 |
             static_cast<uint64_t>(mac[2]) << 24 | static_cast<uint64_t>(mac[3]) << 16 |
             static_cast<uint64_t>(mac[4]) << 8 | static_cast<uint64_t>(mac[5]);
  return true;
}

bool hasPeerStem(const char* name) {
  if (!name || strlen(name) < 19 || strncmp(name, "device_", 7) != 0) return false;
  for (size_t i = 7; i < 19; ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(name[i]))) return false;
  }
  return true;
}

bool canonicalPeerVersion(const char* name, uint16_t& version) {
  if (!hasPeerStem(name) || strncmp(name + 19, "_v", 2) != 0) return false;
  const char* cursor = name + 21;
  if (!std::isdigit(static_cast<unsigned char>(*cursor))) return false;
  uint32_t parsed = 0;
  do {
    const uint8_t digit = static_cast<uint8_t>(*cursor - '0');
    parsed = parsed > UINT16_MAX / 10U ? UINT16_MAX + 1U : parsed * 10U + digit;
    ++cursor;
  } while (std::isdigit(static_cast<unsigned char>(*cursor)));
  if (strcmp(cursor, ".bin") != 0 && strcmp(cursor, ".bin.bak") != 0 && strcmp(cursor, ".bin.tmp") != 0) {
    return false;
  }
  version = static_cast<uint16_t>(std::min<uint32_t>(parsed, UINT16_MAX));
  return true;
}

bool isLegacyPeerFileName(const char* name) {
  if (!hasPeerStem(name)) return false;
  const char* suffix = name + 19;
  return strcmp(suffix, ".bin") == 0 || strcmp(suffix, ".bin.bak") == 0 || strcmp(suffix, ".bin.tmp") == 0;
}

bool peerIdentity(const char* name, uint64_t& identity) {
  if (!hasPeerStem(name)) return false;
  identity = 0;
  for (size_t index = 7; index < 19; ++index) {
    const unsigned char character = static_cast<unsigned char>(name[index]);
    const uint8_t nibble = character >= '0' && character <= '9'
                               ? static_cast<uint8_t>(character - '0')
                               : static_cast<uint8_t>(std::tolower(character) - 'a' + 10);
    identity = identity << 4 | nibble;
  }
  return true;
}

std::string peerStem(const uint64_t identity) {
  constexpr char HEX_DIGITS[] = "0123456789abcdef";
  char stem[20] = "device_000000000000";
  uint64_t remaining = identity;
  for (size_t index = 0; index < 12; ++index) {
    stem[18 - index] = HEX_DIGITS[remaining & 0x0F];
    remaining >>= 4;
  }
  return stem;
}

LoadOutcome loadPeerSnapshot(const std::string& canonicalPath, const std::string& legacyPath,
                             GlobalReadingStats& peer) {
  const bool hasCommittedEnvelope =
      Storage.exists(canonicalPath.c_str()) || Storage.exists((canonicalPath + ".bak").c_str());
  if (hasCommittedEnvelope) {
    return loadPreferredEnvelope(canonicalPath, ReadingStatsEnvelope::Kind::PeerGlobal, peer);
  }
  const LoadOutcome temporary =
      loadEnvelopePath((canonicalPath + ".tmp").c_str(), ReadingStatsEnvelope::Kind::PeerGlobal, peer);
  if (temporary.result == ReadingStatsDecodeResult::Ok || isProtected(temporary)) return temporary;
  // A lone invalid/empty temp never committed. The retained raw snapshot is
  // still authoritative and may be read without modifying either file.
  return loadPreferredLegacy(legacyPath, peer);
}

GlobalReadingStats::LoadStatus protectedLoadStatus(const LoadOutcome& outcome) {
  if (outcome.result == ReadingStatsDecodeResult::NewerFormat) return GlobalReadingStats::LoadStatus::NewerFormat;
  if (outcome.readResult == ReadingStatsStorage::ReadResult::IoError ||
      outcome.readResult == ReadingStatsStorage::ReadResult::TooLarge) {
    return GlobalReadingStats::LoadStatus::IoError;
  }
  return GlobalReadingStats::LoadStatus::Invalid;
}

bool legacyGlobalFilesAllowReset() {
  const std::array<std::string, 3> paths = {LEGACY_GLOBAL_STATS_PATH, std::string(LEGACY_GLOBAL_STATS_PATH) + ".bak",
                                            std::string(LEGACY_GLOBAL_STATS_PATH) + ".tmp"};
  return std::all_of(paths.begin(), paths.end(), [](const std::string& path) {
    GlobalReadingStats ignored;
    const LoadOutcome outcome = loadLegacyPath(path.c_str(), ignored);
    if (!isProtected(outcome)) return true;
    LOG_ERR(LOG_TAG, "Refusing to shadow newer or unreadable legacy global stats during reset: %s", path.c_str());
    return false;
  });
}

ReadingStatsVersionGuard::Result scanForNewerGlobalCanonicalFile() {
  return ReadingStatsVersionGuard::scan(GLOBAL_STATS_DIRECTORY, "global_stats_v", CURRENT_GLOBAL_CANONICAL_VERSION);
}

bool isExactPayload(const LoadOutcome& outcome, const GlobalReadingStats& stats,
                    const ReadingStatsCodec::GlobalBytes& expected) {
  return outcome.result == ReadingStatsDecodeResult::Ok && ReadingStatsCodec::encode(stats) == expected;
}

bool storageAllowsPublish() {
  if (scanForNewerGlobalCanonicalFile() != ReadingStatsVersionGuard::Result::NoNewerFile) return false;
  GlobalReadingStats ignored;
  return !isProtected(loadEnvelopePath(GLOBAL_STATS_PATH, ReadingStatsEnvelope::Kind::Global, ignored)) &&
         !isProtected(loadEnvelopePath(GLOBAL_STATS_BACKUP_PATH, ReadingStatsEnvelope::Kind::Global, ignored)) &&
         !isProtected(loadEnvelopePath(GLOBAL_STATS_TEMP_PATH, ReadingStatsEnvelope::Kind::Global, ignored));
}

GlobalReadingStats::BackupResult backupResultFor(const LoadOutcome& outcome) {
  if (outcome.result == ReadingStatsDecodeResult::NewerFormat) {
    return GlobalReadingStats::BackupResult::NewerFormat;
  }
  if (outcome.wrongKind) return GlobalReadingStats::BackupResult::Protected;
  if (outcome.readResult == ReadingStatsStorage::ReadResult::Missing) {
    return GlobalReadingStats::BackupResult::Missing;
  }
  if (outcome.readResult == ReadingStatsStorage::ReadResult::IoError ||
      outcome.readResult == ReadingStatsStorage::ReadResult::TooLarge) {
    return GlobalReadingStats::BackupResult::IoError;
  }
  return GlobalReadingStats::BackupResult::Invalid;
}
}  // namespace

GlobalReadingStats GlobalReadingStats::load(LoadStatus* status) {
  const auto finish = [status](const LoadStatus result) {
    if (status) *status = result;
  };
  const ReadingStatsVersionGuard::Result versionGuard = scanForNewerGlobalCanonicalFile();
  if (versionGuard != ReadingStatsVersionGuard::Result::NoNewerFile) {
    LOG_ERR(LOG_TAG, "A newer or unreadable global stats sibling exists; refusing the v4 view");
    finish(versionGuard == ReadingStatsVersionGuard::Result::NewerFile ? LoadStatus::NewerFormat : LoadStatus::IoError);
    return {};
  }
  GlobalReadingStats stats;
  const LoadOutcome primary = loadEnvelopePath(GLOBAL_STATS_PATH, ReadingStatsEnvelope::Kind::Global, stats);
  if (primary.result == ReadingStatsDecodeResult::Ok) {
    loadDailySummary(stats);
    finish(LoadStatus::Ok);
    return stats;
  }
  if (isProtected(primary)) {
    LOG_ERR(LOG_TAG, "Unreadable, wrong-kind, or newer global stats detected; refusing an older fallback");
    finish(protectedLoadStatus(primary));
    return {};
  }

  const LoadOutcome backup = loadEnvelopePath(GLOBAL_STATS_BACKUP_PATH, ReadingStatsEnvelope::Kind::Global, stats);
  if (backup.result == ReadingStatsDecodeResult::Ok) {
    LOG_INF(LOG_TAG, "Recovered global stats from backup");
    loadDailySummary(stats);
    finish(LoadStatus::RecoveredBackup);
    return stats;
  }
  if (isProtected(backup)) {
    LOG_ERR(LOG_TAG, "Unreadable, wrong-kind, or newer global stats backup detected; refusing a temporary fallback");
    finish(protectedLoadStatus(backup));
    return {};
  }

  const LoadOutcome temporary = loadEnvelopePath(GLOBAL_STATS_TEMP_PATH, ReadingStatsEnvelope::Kind::Global, stats);
  if (temporary.result == ReadingStatsDecodeResult::Ok) {
    LOG_INF(LOG_TAG, "Recovered global stats from interrupted write");
    loadDailySummary(stats);
    finish(LoadStatus::RecoveredTemp);
    return stats;
  }
  if (isProtected(temporary)) {
    LOG_ERR(LOG_TAG, "Unreadable, wrong-kind, or newer global stats temp detected; leaving it untouched");
    finish(protectedLoadStatus(temporary));
    return {};
  }

  const bool hasCommittedEnvelopeArtifact = primary.readResult != ReadingStatsStorage::ReadResult::Missing ||
                                            backup.readResult != ReadingStatsStorage::ReadResult::Missing;
  if (hasCommittedEnvelopeArtifact) {
    finish(LoadStatus::Invalid);
    return {};
  }

  const LoadOutcome legacy = loadPreferredLegacy(LEGACY_GLOBAL_STATS_PATH, stats);
  if (legacy.result == ReadingStatsDecodeResult::Ok) {
    loadDailySummary(stats);
    if (!stats.save()) {
      LOG_ERR(LOG_TAG, "Loaded CrossInk global stats but could not publish the CrossVi envelope");
    } else {
      LOG_INF(LOG_TAG, "Migrated CrossInk global stats without modifying the source");
    }
    finish(LoadStatus::LoadedLegacy);
    return stats;
  }
  if (isProtected(legacy)) {
    LOG_ERR(LOG_TAG, "Unreadable or newer CrossInk global stats detected; leaving them untouched");
    finish(protectedLoadStatus(legacy));
    return {};
  }
  finish(legacy.readResult == ReadingStatsStorage::ReadResult::Missing ? LoadStatus::Missing : LoadStatus::Invalid);
  return {};
}

bool GlobalReadingStats::hasSyncedStats() { return Storage.exists(SYNCED_STATS_DIR); }

bool GlobalReadingStats::canPublish() { return storageAllowsPublish(); }

GlobalReadingStats GlobalReadingStats::loadAggregated() { return loadAggregatedWithReport(load()).stats; }

GlobalReadingStats GlobalReadingStats::loadAggregated(const GlobalReadingStats& localStats) {
  return loadAggregatedWithReport(localStats).stats;
}

GlobalReadingStatsAggregation GlobalReadingStats::loadAggregatedWithReport(const GlobalReadingStats& localStats) {
  GlobalReadingStatsAggregation report;
  report.stats = localStats;
  HalFile directory = Storage.open(SYNCED_STATS_DIR);
  if (!directory || !directory.isDirectory()) {
    // HalStorage cannot distinguish a missing path from an SD open failure.
    // Mark both fail-closed; callers that have no valid peer hide the combined
    // page anyway, while a future caller cannot mistake this for a full scan.
    report.scanComplete = false;
    if (directory) directory.close();
    return report;
  }

  uint64_t localIdentity = 0;
  if (!localSyncedStatsIdentity(localIdentity)) {
    report.scanComplete = false;
    LOG_ERR(LOG_TAG, "Local device identity unavailable; refusing a possibly self-inclusive aggregate");
    directory.close();
    return report;
  }
  std::array<uint64_t, GlobalReadingStats::MAX_SYNCED_DEVICE_SNAPSHOTS> processedPeers{};
  std::array<bool, GlobalReadingStats::MAX_SYNCED_DEVICE_SNAPSHOTS> hasNewerPeerFile{};
  size_t processedPeerCount = 0;
  char name[128]{};
  const auto closeEntry = [&report](HalFile& file) {
    if (!file.close()) {
      report.scanComplete = false;
      LOG_ERR(LOG_TAG, "Synced stats entry close failed; aggregate is incomplete");
    }
  };
  while (true) {
    HalFile file = directory.openNextFile();
    if (!file) {
      if (directory.getError() != 0) {
        report.scanComplete = false;
        LOG_ERR(LOG_TAG, "Synced stats directory scan failed; aggregate is incomplete");
      }
      break;
    }
    const bool isDirectory = file.isDirectory();
    const size_t nameLength = file.getName(name, sizeof(name));
    if (nameLength == 0) {
      report.scanComplete = false;
      LOG_ERR(LOG_TAG, "Synced stats entry name could not be read; aggregate is incomplete");
      closeEntry(file);
      continue;
    }
    uint16_t canonicalVersion = 0;
    const bool canonical = canonicalPeerVersion(name, canonicalVersion);
    if (isDirectory || (!canonical && !isLegacyPeerFileName(name)) ||
        (canonical && canonicalVersion < CURRENT_GLOBAL_CANONICAL_VERSION)) {
      closeEntry(file);
      continue;
    }

    uint64_t identity = 0;
    const bool hasIdentity = peerIdentity(name, identity);
    closeEntry(file);
    if (!hasIdentity || identity == localIdentity) continue;
    const auto found = std::find(processedPeers.begin(), processedPeers.begin() + processedPeerCount, identity);
    if (found != processedPeers.begin() + processedPeerCount) {
      const size_t index = static_cast<size_t>(found - processedPeers.begin());
      hasNewerPeerFile[index] = hasNewerPeerFile[index] || canonicalVersion > CURRENT_GLOBAL_CANONICAL_VERSION;
      continue;
    }
    if (processedPeerCount == processedPeers.size()) {
      report.scanComplete = false;
      LOG_ERR(LOG_TAG, "Synced stats peer limit reached; aggregate is incomplete");
      break;
    }
    processedPeers[processedPeerCount] = identity;
    hasNewerPeerFile[processedPeerCount] = canonicalVersion > CURRENT_GLOBAL_CANONICAL_VERSION;
    ++processedPeerCount;
  }
  if (!directory.close()) {
    report.scanComplete = false;
    LOG_ERR(LOG_TAG, "Synced stats directory close failed; aggregate is incomplete");
  }

  for (size_t index = 0; index < processedPeerCount; ++index) {
    const uint64_t identity = processedPeers[index];
    if (hasNewerPeerFile[index]) {
      report.scanComplete = false;
      if (report.skippedPeerCount < UINT16_MAX) ++report.skippedPeerCount;
      LOG_ERR(LOG_TAG, "A newer peer stats sibling exists; refusing the v4/legacy snapshot");
      continue;
    }
    const std::string base = std::string(SYNCED_STATS_DIR) + "/" + peerStem(identity);
    const std::string canonicalPath = base + "_v4.bin";
    const std::string legacyPath = base + ".bin";
    GlobalReadingStats peer;
    const LoadOutcome loaded = loadPeerSnapshot(canonicalPath, legacyPath, peer);
    if (loaded.result == ReadingStatsDecodeResult::Ok) {
      report.stats.merge(peer);
      if (report.validPeerCount < UINT16_MAX) ++report.validPeerCount;
    } else {
      if (report.skippedPeerCount < UINT16_MAX) ++report.skippedPeerCount;
    }
  }
  if (report.validPeerCount > 0 || report.skippedPeerCount > 0 || !report.scanComplete) {
    LOG_INF(LOG_TAG, "Aggregated %u synced stats file(s), skipped %u, complete=%u",
            static_cast<unsigned>(report.validPeerCount), static_cast<unsigned>(report.skippedPeerCount),
            static_cast<unsigned>(report.scanComplete));
  }
  return report;
}

bool GlobalReadingStats::save() const {
  if (!ReadingStatsCompletionTransaction::permitsGlobalWrite(*this)) {
    LOG_ERR(LOG_TAG, "Refusing to overwrite a pending completion transaction");
    return false;
  }
  if (!storageAllowsPublish()) {
    LOG_ERR(LOG_TAG, "Refusing to shadow protected global stats storage");
    return false;
  }
  GlobalReadingStats existing;
  const LoadOutcome primary = loadEnvelopePath(GLOBAL_STATS_PATH, ReadingStatsEnvelope::Kind::Global, existing);
  if (isProtected(primary)) {
    LOG_ERR(LOG_TAG, "Refusing to overwrite unreadable or newer global stats");
    return false;
  }

  const ReadingStatsCodec::GlobalBytes data = ReadingStatsCodec::encode(*this);
  if (!ReadingStatsEnvelope::writeAtomic(GLOBAL_STATS_PATH, GLOBAL_STATS_BACKUP_PATH,
                                         primary.result == ReadingStatsDecodeResult::Ok,
                                         ReadingStatsEnvelope::Kind::Global, data.data(), data.size())) {
    return false;
  }
  // The daily sidecar is supplemental: a failure hides "Today" but must not
  // turn a successfully committed cumulative-statistics write into a retry
  // loop. Its fingerprint prevents stale data from being displayed.
  if (!saveDailySummary(*this)) LOG_ERR(LOG_TAG, "Could not save daily reading summary");
  return true;
}

bool GlobalReadingStats::saveRedundant() const {
  if (scanForNewerGlobalCanonicalFile() != ReadingStatsVersionGuard::Result::NoNewerFile) return false;
  const ReadingStatsCodec::GlobalBytes expected = ReadingStatsCodec::encode(*this);
  GlobalReadingStats primaryStats;
  LoadOutcome primary = loadEnvelopePath(GLOBAL_STATS_PATH, ReadingStatsEnvelope::Kind::Global, primaryStats);
  if (isProtected(primary)) return false;
  GlobalReadingStats backupStats;
  LoadOutcome backup = loadEnvelopePath(GLOBAL_STATS_BACKUP_PATH, ReadingStatsEnvelope::Kind::Global, backupStats);
  if (isProtected(backup)) return false;
  if (isExactPayload(primary, primaryStats, expected) && isExactPayload(backup, backupStats, expected)) return true;

  if (!isExactPayload(primary, primaryStats, expected)) {
    if (!save()) return false;
    primary = loadEnvelopePath(GLOBAL_STATS_PATH, ReadingStatsEnvelope::Kind::Global, primaryStats);
    backup = loadEnvelopePath(GLOBAL_STATS_BACKUP_PATH, ReadingStatsEnvelope::Kind::Global, backupStats);
    if (isProtected(primary) || isProtected(backup)) return false;
    if (isExactPayload(primary, primaryStats, expected) && isExactPayload(backup, backupStats, expected)) return true;
  }
  if (!save()) return false;

  primary = loadEnvelopePath(GLOBAL_STATS_PATH, ReadingStatsEnvelope::Kind::Global, primaryStats);
  backup = loadEnvelopePath(GLOBAL_STATS_BACKUP_PATH, ReadingStatsEnvelope::Kind::Global, backupStats);
  return isExactPayload(primary, primaryStats, expected) && isExactPayload(backup, backupStats, expected);
}

bool GlobalReadingStats::resetLocal() {
  if (!ReadingStatsCompletionTransaction::canResetGlobalStats()) {
    LOG_ERR(LOG_TAG, "Refusing to reset global stats with a pending completion transaction");
    return false;
  }
  if (scanForNewerGlobalCanonicalFile() != ReadingStatsVersionGuard::Result::NoNewerFile) {
    LOG_ERR(LOG_TAG, "Refusing to reset beside a newer or unreadable global stats sibling");
    return false;
  }
  if (!legacyGlobalFilesAllowReset()) return false;

  GlobalReadingStats ignored;
  const LoadOutcome primary = loadEnvelopePath(GLOBAL_STATS_PATH, ReadingStatsEnvelope::Kind::Global, ignored);
  const LoadOutcome backup = loadEnvelopePath(GLOBAL_STATS_BACKUP_PATH, ReadingStatsEnvelope::Kind::Global, ignored);
  const LoadOutcome temporary = loadEnvelopePath(GLOBAL_STATS_TEMP_PATH, ReadingStatsEnvelope::Kind::Global, ignored);
  if (isProtected(primary) || isProtected(backup) || isProtected(temporary)) {
    LOG_ERR(LOG_TAG, "Refusing to replace protected global stats during reset");
    return false;
  }

  const GlobalReadingStats zero;
  const ReadingStatsCodec::GlobalBytes tombstone = ReadingStatsCodec::encode(zero);
  if (!ReadingStatsEnvelope::writeAtomic(GLOBAL_STATS_BACKUP_PATH, nullptr, false, ReadingStatsEnvelope::Kind::Global,
                                         tombstone.data(), tombstone.size()) ||
      !ReadingStatsEnvelope::writeAtomic(GLOBAL_STATS_PATH, nullptr, false, ReadingStatsEnvelope::Kind::Global,
                                         tombstone.data(), tombstone.size())) {
    return false;
  }
  GlobalReadingStats primaryStats;
  GlobalReadingStats backupStats;
  const LoadOutcome verifiedPrimary =
      loadEnvelopePath(GLOBAL_STATS_PATH, ReadingStatsEnvelope::Kind::Global, primaryStats);
  const LoadOutcome verifiedBackup =
      loadEnvelopePath(GLOBAL_STATS_BACKUP_PATH, ReadingStatsEnvelope::Kind::Global, backupStats);
  return isExactPayload(verifiedPrimary, primaryStats, tombstone) &&
         isExactPayload(verifiedBackup, backupStats, tombstone);
}

GlobalReadingStats::BackupResult GlobalReadingStats::createBackup() {
  LoadStatus sourceStatus = LoadStatus::Missing;
  const GlobalReadingStats source = load(&sourceStatus);
  if (sourceStatus == LoadStatus::Missing) return BackupResult::Missing;
  if (!isTrustedLoadStatus(sourceStatus)) {
    if (sourceStatus == LoadStatus::NewerFormat) return BackupResult::NewerFormat;
    if (sourceStatus == LoadStatus::IoError) return BackupResult::IoError;
    return BackupResult::Invalid;
  }

  if ((!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) ||
      (!Storage.exists(USER_BACKUP_DIRECTORY) && !Storage.mkdir(USER_BACKUP_DIRECTORY))) {
    return BackupResult::IoError;
  }

  GlobalReadingStats ignored;
  const LoadOutcome primary = loadEnvelopePath(USER_BACKUP_PATH, ReadingStatsEnvelope::Kind::Global, ignored);
  const LoadOutcome previous =
      loadEnvelopePath(USER_BACKUP_PREVIOUS_PATH, ReadingStatsEnvelope::Kind::Global, ignored);
  const LoadOutcome temporary =
      loadEnvelopePath(USER_BACKUP_TEMP_PATH, ReadingStatsEnvelope::Kind::Global, ignored);
  if (isProtected(primary) || isProtected(previous) || isProtected(temporary)) return BackupResult::Protected;

  const ReadingStatsCodec::GlobalBytes data = ReadingStatsCodec::encode(source);
  const bool rotateExisting = primary.result == ReadingStatsDecodeResult::Ok;
  if (!ReadingStatsEnvelope::writeAtomic(USER_BACKUP_PATH, USER_BACKUP_PREVIOUS_PATH, rotateExisting,
                                         ReadingStatsEnvelope::Kind::Global, data.data(), data.size())) {
    return BackupResult::IoError;
  }
  return BackupResult::Ok;
}

GlobalReadingStats::BackupResult GlobalReadingStats::restoreBackup() {
  GlobalReadingStats restored;
  const LoadOutcome loaded =
      loadPreferredEnvelope(USER_BACKUP_PATH, ReadingStatsEnvelope::Kind::Global, restored);
  if (loaded.result != ReadingStatsDecodeResult::Ok) return backupResultFor(loaded);
  if (!restored.saveRedundant()) return BackupResult::Protected;

  LoadStatus status = LoadStatus::Invalid;
  const GlobalReadingStats verified = load(&status);
  if (!isTrustedLoadStatus(status) || ReadingStatsCodec::encode(verified) != ReadingStatsCodec::encode(restored)) {
    return BackupResult::IoError;
  }
  return BackupResult::Ok;
}

bool GlobalReadingStats::hasValidBackup() {
  GlobalReadingStats ignored;
  return loadPreferredEnvelope(USER_BACKUP_PATH, ReadingStatsEnvelope::Kind::Global, ignored).result ==
         ReadingStatsDecodeResult::Ok;
}
