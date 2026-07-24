#include "PerBookReaderSettingsStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <string>

#include "PerBookReaderSettingsCodec.h"

namespace PerBookReaderSettingsStore {
namespace {

using DecodeStatus = PerBookReaderSettingsCodec::DecodeStatus;

enum class ReadStatus : uint8_t { OK, MISSING, NEWER_VERSION, INVALID, IO_ERROR };
enum class LegacyReadStatus : uint8_t { OK, MISSING, NEWER_VERSION, INVALID, IO_ERROR };
enum class ExactStatus : uint8_t { MATCH, MISMATCH, IO_ERROR };
enum class BackupStatus : uint8_t { OK, CONFLICT, IO_ERROR };

constexpr size_t CROSSINK_V1_SIZE = 3;
constexpr size_t CROSSINK_V2_SIZE = 86;
constexpr uint8_t CROSSINK_V1 = 1;
constexpr uint8_t CROSSINK_V2 = 2;
constexpr uint8_t CROSSINK_CUSTOM = 1U << 0;
constexpr uint8_t CROSSINK_INTERVAL = 1U << 1;
constexpr uint8_t CROSSINK_RENDER_MODE = 1U << 2;

struct LegacySettings {
  uint8_t version = 0;
  std::array<uint8_t, CROSSINK_V2_SIZE> bytes{};
  size_t size = 0;
  PerBookReaderSettings mapped;
};

std::string makeNamedPath(const std::string& cachePath, const char* name) {
  return cachePath + (cachePath.empty() || cachePath.back() != '/' ? "/" : "") + name;
}

std::string makePath(const std::string& cachePath, const char* suffix) {
  return makeNamedPath(cachePath, FILE_NAME) + suffix;
}

bool validLegacyFontName(const uint8_t* name) {
  size_t length = 0;
  while (length < 64 && name[length] != 0) ++length;
  if (length == 64 || !PerBookReaderSettingsCodec::isValidUtf8({reinterpret_cast<const char*>(name), length})) {
    return false;
  }
  return std::all_of(name + length, name + 64, [](const uint8_t value) { return value == 0; });
}

void mapLineHeight(const uint8_t percent, PerBookReaderSettings& settings) {
  const bool sans = settings.sdFontFamilyName.front() == '\0' && settings.fontFamily == 1;
  const std::array<uint8_t, 3> exact =
      sans ? std::array<uint8_t, 3>{90, 95, 100} : std::array<uint8_t, 3>{95, 100, 110};
  const auto found = std::find(exact.begin(), exact.end(), percent);
  if (found != exact.end()) settings.lineSpacing = static_cast<uint8_t>(found - exact.begin());
}

bool validLegacyV2(const uint8_t* bytes) {
  const auto isToggle = [](const uint8_t value) { return value <= 1; };
  const uint8_t flags = bytes[1];
  const uint16_t seconds = PerBookReaderSettingsCodec::readU16(bytes + 2);
  const bool validSeconds = seconds == 0 || (seconds >= 5 && seconds <= 120);
  return (flags & ~(CROSSINK_CUSTOM | CROSSINK_INTERVAL | CROSSINK_RENDER_MODE)) == 0 && validSeconds && bytes[4] < 3 &&
         bytes[5] < 2 && bytes[6] < 8 && bytes[7] >= 70 && bytes[7] <= 200 && bytes[8] < 4 && bytes[9] >= 5 &&
         bytes[9] <= 40 && isToggle(bytes[10]) && bytes[11] < 5 && isToggle(bytes[12]) && isToggle(bytes[13]) &&
         isToggle(bytes[14]) && isToggle(bytes[15]) && bytes[16] < 3 && isToggle(bytes[17]) && isToggle(bytes[18]) &&
         isToggle(bytes[19]) && isToggle(bytes[20]) && bytes[21] < 3 && validLegacyFontName(bytes + 22);
}

LegacyReadStatus readLegacy(const std::string& path, const PerBookReaderSettings& defaults, LegacySettings& legacy) {
  if (!Storage.exists(path.c_str())) return LegacyReadStatus::MISSING;
  HalFile file;
  if (!Storage.openFileForRead("PBRS", path, file)) return LegacyReadStatus::IO_ERROR;
  const uint64_t size = file.fileSize64();
  if (size == 0) {
    file.close();
    return LegacyReadStatus::INVALID;
  }
  uint8_t version = 0;
  if (file.read(&version, 1) != 1) {
    file.close();
    return LegacyReadStatus::IO_ERROR;
  }
  if (version > CROSSINK_V2) {
    file.close();
    return LegacyReadStatus::NEWER_VERSION;
  }
  const size_t expectedSize = version == CROSSINK_V1 ? CROSSINK_V1_SIZE : CROSSINK_V2_SIZE;
  if ((version != CROSSINK_V1 && version != CROSSINK_V2) || size != expectedSize) {
    file.close();
    return LegacyReadStatus::INVALID;
  }
  if (!file.seek(0) || file.read(legacy.bytes.data(), expectedSize) != static_cast<int>(expectedSize) ||
      !file.close()) {
    return LegacyReadStatus::IO_ERROR;
  }

  legacy.version = version;
  legacy.size = expectedSize;
  legacy.mapped = defaults;
  legacy.mapped.hasReaderOverrides = false;
  legacy.mapped.hasAutoPageTurnInterval = false;
  legacy.mapped.autoPageTurnStartsOnOpen = false;
  legacy.mapped.autoPageTurnSeconds = 0;
  const uint16_t seconds = PerBookReaderSettingsCodec::readU16(legacy.bytes.data() + (version == CROSSINK_V1 ? 1 : 2));
  if ((seconds != 0 && (seconds < 5 || seconds > 120)) ||
      (version == CROSSINK_V2 && !validLegacyV2(legacy.bytes.data()))) {
    return LegacyReadStatus::INVALID;
  }
  if (version == CROSSINK_V1) {
    if (seconds != 0) {
      legacy.mapped.hasAutoPageTurnInterval = true;
      legacy.mapped.autoPageTurnSeconds = static_cast<uint8_t>(seconds);
    }
    return LegacyReadStatus::OK;
  }

  const uint8_t flags = legacy.bytes[1];
  legacy.mapped.hasReaderOverrides = (flags & CROSSINK_CUSTOM) != 0;
  if ((flags & CROSSINK_INTERVAL) != 0 && seconds != 0) {
    legacy.mapped.hasAutoPageTurnInterval = true;
    legacy.mapped.autoPageTurnSeconds = static_cast<uint8_t>(seconds);
  }
  if (legacy.mapped.hasReaderOverrides) {
    mapLineHeight(legacy.bytes[7], legacy.mapped);
    legacy.mapped.orientation = legacy.bytes[8];
    legacy.mapped.screenMargin = legacy.bytes[9];
    legacy.mapped.paragraphAlignment = legacy.bytes[11];
    legacy.mapped.embeddedStyle = legacy.bytes[12];
    legacy.mapped.hyphenationEnabled = legacy.bytes[13];
    legacy.mapped.textAntiAliasing = legacy.bytes[14];
    legacy.mapped.imageRendering = legacy.bytes[16];
    legacy.mapped.extraParagraphSpacing = legacy.bytes[17];
    legacy.mapped.forceParagraphIndents = legacy.bytes[18];
    legacy.mapped.focusReadingEnabled = legacy.bytes[19];
  }
  if ((flags & CROSSINK_RENDER_MODE) != 0) {
    legacy.mapped.hasRenderModeOverride = true;
    legacy.mapped.renderMode = static_cast<EpubRenderMode>(legacy.bytes[4]);
  }
  return LegacyReadStatus::OK;
}

ExactStatus exactBytes(const std::string& path, const uint8_t* expected, const size_t length) {
  HalFile file;
  if (!Storage.openFileForRead("PBRS", path, file)) return ExactStatus::IO_ERROR;
  if (file.fileSize64() != length) {
    file.close();
    return ExactStatus::MISMATCH;
  }
  std::array<uint8_t, CROSSINK_V2_SIZE> actual{};
  if (file.read(actual.data(), length) != static_cast<int>(length) || !file.close()) return ExactStatus::IO_ERROR;
  return std::equal(actual.begin(), actual.begin() + static_cast<std::ptrdiff_t>(length), expected)
             ? ExactStatus::MATCH
             : ExactStatus::MISMATCH;
}

BackupStatus createLegacyBackup(const std::string& legacyPath, const LegacySettings& legacy) {
  const std::string backupPath = legacyPath + ".crossink-v" + std::to_string(legacy.version) + ".orig";
  const std::string tempPath = backupPath + ".tmp";
  if (Storage.exists(backupPath.c_str())) {
    const ExactStatus exact = exactBytes(backupPath, legacy.bytes.data(), legacy.size);
    if (exact != ExactStatus::MATCH) {
      return exact == ExactStatus::MISMATCH ? BackupStatus::CONFLICT : BackupStatus::IO_ERROR;
    }
    if (Storage.exists(tempPath.c_str())) {
      const ExactStatus tempExact = exactBytes(tempPath, legacy.bytes.data(), legacy.size);
      if (tempExact != ExactStatus::MATCH) {
        return tempExact == ExactStatus::MISMATCH ? BackupStatus::CONFLICT : BackupStatus::IO_ERROR;
      }
    }
    return BackupStatus::OK;
  }

  if (Storage.exists(tempPath.c_str())) {
    const ExactStatus exact = exactBytes(tempPath, legacy.bytes.data(), legacy.size);
    if (exact != ExactStatus::MATCH) {
      return exact == ExactStatus::MISMATCH ? BackupStatus::CONFLICT : BackupStatus::IO_ERROR;
    }
  } else {
    HalFile temp;
    if (!Storage.openFileForWrite("PBRS", tempPath, temp)) return BackupStatus::IO_ERROR;
    bool ok = temp.write(legacy.bytes.data(), legacy.size) == legacy.size;
    temp.flush();
    if (ok) ok = temp.sync();
    if (!temp.close()) ok = false;
    if (!ok || exactBytes(tempPath, legacy.bytes.data(), legacy.size) != ExactStatus::MATCH) {
      Storage.remove(tempPath.c_str());
      return BackupStatus::IO_ERROR;
    }
  }
  if (!Storage.rename(tempPath.c_str(), backupPath.c_str()) ||
      exactBytes(backupPath, legacy.bytes.data(), legacy.size) != ExactStatus::MATCH) {
    return BackupStatus::IO_ERROR;
  }
  return exactBytes(legacyPath, legacy.bytes.data(), legacy.size) == ExactStatus::MATCH ? BackupStatus::OK
                                                                                        : BackupStatus::IO_ERROR;
}

ReadStatus readStored(const std::string& path, PerBookReaderSettings& settings) {
  if (!Storage.exists(path.c_str())) return ReadStatus::MISSING;

  HalFile file;
  if (!Storage.openFileForRead("PBRS", path, file)) return ReadStatus::IO_ERROR;

  const size_t fileSize = file.fileSize();
  if (fileSize > PerBookReaderSettingsCodec::ENCODED_SIZE) {
    std::array<uint8_t, PerBookReaderSettingsCodec::VERSION_OFFSET + 1> prefix{};
    if (file.read(prefix.data(), prefix.size()) != static_cast<int>(prefix.size())) return ReadStatus::IO_ERROR;
    if (std::equal(PerBookReaderSettingsCodec::MAGIC.begin(), PerBookReaderSettingsCodec::MAGIC.end(),
                   prefix.begin()) &&
        prefix[PerBookReaderSettingsCodec::VERSION_OFFSET] > PerBookReaderSettingsCodec::VERSION) {
      return ReadStatus::NEWER_VERSION;
    }
    return ReadStatus::INVALID;
  }

  PerBookReaderSettingsCodec::Encoded encoded{};
  if (file.read(encoded.data(), fileSize) != static_cast<int>(fileSize)) return ReadStatus::IO_ERROR;

  const DecodeStatus status = PerBookReaderSettingsCodec::decode(encoded.data(), fileSize, settings);
  if (status == DecodeStatus::OK) return ReadStatus::OK;
  if (status == DecodeStatus::NEWER_VERSION) return ReadStatus::NEWER_VERSION;
  return ReadStatus::INVALID;
}

bool removeIfPresent(const std::string& path) { return !Storage.exists(path.c_str()) || Storage.remove(path.c_str()); }

bool writeVerified(const std::string& path, const PerBookReaderSettingsCodec::Encoded& encoded,
                   const PerBookReaderSettings& settings) {
  HalFile file;
  if (!Storage.openFileForWrite("PBRS", path, file)) return false;
  if (file.write(encoded.data(), encoded.size()) != encoded.size()) {
    file.close();
    return false;
  }
  file.flush();
  if (!file.sync() || !file.close()) return false;
  PerBookReaderSettings verified;
  return readStored(path, verified) == ReadStatus::OK && verified == settings;
}

}  // namespace

LoadStatus load(const std::string& cachePath, PerBookReaderSettings& settings) {
  const std::string finalPath = makePath(cachePath, "");
  const std::string backupPath = makePath(cachePath, ".bak");
  const std::string tempPath = makePath(cachePath, ".tmp");

  const ReadStatus finalStatus = readStored(finalPath, settings);
  if (finalStatus == ReadStatus::OK) return LoadStatus::LOADED;
  if (finalStatus == ReadStatus::NEWER_VERSION) return LoadStatus::NEWER_VERSION;
  if (finalStatus == ReadStatus::IO_ERROR) return LoadStatus::IO_ERROR;

  const ReadStatus backupStatus = readStored(backupPath, settings);
  if (backupStatus == ReadStatus::OK) return LoadStatus::LOADED_BACKUP;
  if (backupStatus == ReadStatus::NEWER_VERSION) return LoadStatus::NEWER_VERSION;
  if (backupStatus == ReadStatus::IO_ERROR) return LoadStatus::IO_ERROR;

  // A synced .tmp may be the only complete copy after power loss. Committed
  // files always win, but never discard a valid (or newer) temporary copy when
  // neither canonical nor backup can be used.
  const ReadStatus tempStatus = readStored(tempPath, settings);
  if (tempStatus == ReadStatus::OK) return LoadStatus::LOADED_TEMP;
  if (tempStatus == ReadStatus::NEWER_VERSION) return LoadStatus::NEWER_VERSION;
  if (tempStatus == ReadStatus::IO_ERROR) return LoadStatus::IO_ERROR;
  if (finalStatus == ReadStatus::MISSING && backupStatus == ReadStatus::MISSING && tempStatus == ReadStatus::MISSING) {
    return LoadStatus::MISSING;
  }
  return LoadStatus::INVALID;
}

SaveStatus save(const std::string& cachePath, const PerBookReaderSettings& settings) {
  PerBookReaderSettingsCodec::Encoded encoded;
  if (!PerBookReaderSettingsCodec::encode(settings, encoded)) return SaveStatus::INVALID_SETTINGS;

  const std::string finalPath = makePath(cachePath, "");
  const std::string tempPath = makePath(cachePath, ".tmp");
  const std::string backupPath = makePath(cachePath, ".bak");

  const std::array<const std::string*, 3> paths = {&finalPath, &tempPath, &backupPath};
  std::array<ReadStatus, 3> existingStatuses;
  for (size_t i = 0; i < paths.size(); ++i) {
    PerBookReaderSettings ignored;
    existingStatuses[i] = readStored(*paths[i], ignored);
    if (existingStatuses[i] == ReadStatus::NEWER_VERSION) {
      LOG_ERR("PBRS", "Refusing to overwrite newer per-book settings format: %s", paths[i]->c_str());
      return SaveStatus::NEWER_VERSION;
    }
    if (existingStatuses[i] == ReadStatus::IO_ERROR) return SaveStatus::IO_ERROR;
  }

  const bool finalWasValid = existingStatuses[0] == ReadStatus::OK;

  if (!removeIfPresent(tempPath)) return SaveStatus::IO_ERROR;
  if (!writeVerified(tempPath, encoded, settings)) {
    removeIfPresent(tempPath);
    return SaveStatus::IO_ERROR;
  }

  if (finalWasValid) {
    if (!removeIfPresent(backupPath) || !Storage.rename(finalPath.c_str(), backupPath.c_str())) {
      removeIfPresent(tempPath);
      return SaveStatus::IO_ERROR;
    }
  } else if (!removeIfPresent(finalPath)) {
    removeIfPresent(tempPath);
    return SaveStatus::IO_ERROR;
  }

  if (!Storage.rename(tempPath.c_str(), finalPath.c_str())) {
    if (finalWasValid && !Storage.exists(finalPath.c_str())) {
      Storage.rename(backupPath.c_str(), finalPath.c_str());
    }
    removeIfPresent(tempPath);
    return SaveStatus::IO_ERROR;
  }
  PerBookReaderSettings published;
  if (readStored(finalPath, published) != ReadStatus::OK || published != settings) {
    // A rename is not proof that the bytes reached the SD card intact. Keep
    // the old verified backup when one exists; on a first save, recreate a
    // verified temporary fallback from the still-resident encoded bytes.
    if (!finalWasValid) {
      removeIfPresent(tempPath);
      if (!writeVerified(tempPath, encoded, settings)) removeIfPresent(tempPath);
    }
    return SaveStatus::IO_ERROR;
  }
  return SaveStatus::SAVED;
}

bool clear(const std::string& cachePath) {
  const std::array<std::string, 3> paths = {makePath(cachePath, ""), makePath(cachePath, ".tmp"),
                                            makePath(cachePath, ".bak")};
  // Reset is allowed to discard corrupt/current data, but it must never erase
  // a format written by a newer firmware (including an interrupted .tmp or a
  // fallback .bak), nor proceed when a file cannot be inspected safely.
  if (!std::all_of(paths.begin(), paths.end(), [](const auto& path) {
        PerBookReaderSettings ignored;
        const ReadStatus status = readStored(path, ignored);
        return status != ReadStatus::NEWER_VERSION && status != ReadStatus::IO_ERROR;
      })) {
    return false;
  }
  bool success = true;
  for (const auto& path : paths) success = removeIfPresent(path) && success;
  return success;
}

MigrationStatus migrateCrossInk(const std::string& cachePath, const PerBookReaderSettings& globalDefaults) {
  if (!PerBookReaderSettingsCodec::isValid(globalDefaults)) return MigrationStatus::INVALID_DEFAULTS;

  const std::array<std::string, 3> crossviPaths = {makePath(cachePath, ""), makePath(cachePath, ".bak"),
                                                   makePath(cachePath, ".tmp")};
  if (std::any_of(crossviPaths.begin(), crossviPaths.end(),
                  [](const auto& path) { return Storage.exists(path.c_str()); })) {
    return MigrationStatus::CROSSVI_FILE_PRESENT;
  }

  const std::string legacyPath = makeNamedPath(cachePath, CROSSINK_FILE_NAME);
  LegacySettings legacy;
  switch (readLegacy(legacyPath, globalDefaults, legacy)) {
    case LegacyReadStatus::MISSING:
      return MigrationStatus::NO_LEGACY_FILE;
    case LegacyReadStatus::NEWER_VERSION:
      return MigrationStatus::NEWER_CROSSINK_VERSION;
    case LegacyReadStatus::INVALID:
      return MigrationStatus::INVALID_LEGACY_FILE;
    case LegacyReadStatus::IO_ERROR:
      return MigrationStatus::IO_ERROR;
    case LegacyReadStatus::OK:
      break;
  }
  if (!PerBookReaderSettingsCodec::isValid(legacy.mapped)) return MigrationStatus::INVALID_LEGACY_FILE;

  switch (createLegacyBackup(legacyPath, legacy)) {
    case BackupStatus::CONFLICT:
      return MigrationStatus::BACKUP_CONFLICT;
    case BackupStatus::IO_ERROR:
      return MigrationStatus::IO_ERROR;
    case BackupStatus::OK:
      break;
  }

  if (save(cachePath, legacy.mapped) != SaveStatus::SAVED) return MigrationStatus::SAVE_FAILED;
  PerBookReaderSettings verified;
  return load(cachePath, verified) == LoadStatus::LOADED && verified == legacy.mapped ? MigrationStatus::MIGRATED
                                                                                      : MigrationStatus::SAVE_FAILED;
}

}  // namespace PerBookReaderSettingsStore
