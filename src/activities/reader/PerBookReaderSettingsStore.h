#pragma once

#include <cstdint>
#include <string>

#include "PerBookReaderSettings.h"

namespace PerBookReaderSettingsStore {

// Deliberately distinct from CrossInk's incompatible reader_settings.bin format.
constexpr char FILE_NAME[] = "crossvi_reader_settings.bin";
constexpr char CROSSINK_FILE_NAME[] = "reader_settings.bin";

enum class LoadStatus : uint8_t { LOADED, LOADED_BACKUP, LOADED_TEMP, MISSING, NEWER_VERSION, INVALID, IO_ERROR };
enum class SaveStatus : uint8_t { SAVED, INVALID_SETTINGS, NEWER_VERSION, IO_ERROR };
enum class MigrationStatus : uint8_t {
  MIGRATED,
  NO_LEGACY_FILE,
  CROSSVI_FILE_PRESENT,
  NEWER_CROSSINK_VERSION,
  INVALID_LEGACY_FILE,
  INVALID_DEFAULTS,
  BACKUP_CONFLICT,
  IO_ERROR,
  SAVE_FAILED,
};

LoadStatus load(const std::string& cachePath, PerBookReaderSettings& settings);
SaveStatus save(const std::string& cachePath, const PerBookReaderSettings& settings);
bool clear(const std::string& cachePath);
MigrationStatus migrateCrossInk(const std::string& cachePath, const PerBookReaderSettings& globalDefaults);

}  // namespace PerBookReaderSettingsStore
