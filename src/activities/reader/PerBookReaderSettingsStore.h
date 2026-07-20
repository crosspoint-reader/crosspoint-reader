#pragma once

#include <cstdint>
#include <string>

#include "PerBookReaderSettings.h"

namespace PerBookReaderSettingsStore {

// Deliberately distinct from CrossInk's incompatible reader_settings.bin format.
constexpr char FILE_NAME[] = "crossvi_reader_settings.bin";

enum class LoadStatus : uint8_t { LOADED, LOADED_BACKUP, LOADED_TEMP, MISSING, NEWER_VERSION, INVALID, IO_ERROR };
enum class SaveStatus : uint8_t { SAVED, INVALID_SETTINGS, NEWER_VERSION, IO_ERROR };

LoadStatus load(const std::string& cachePath, PerBookReaderSettings& settings);
SaveStatus save(const std::string& cachePath, const PerBookReaderSettings& settings);
bool clear(const std::string& cachePath);

}  // namespace PerBookReaderSettingsStore
