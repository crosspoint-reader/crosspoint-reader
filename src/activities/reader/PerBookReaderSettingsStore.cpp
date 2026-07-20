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

std::string makePath(const std::string& cachePath, const char* suffix) {
  return cachePath + (cachePath.empty() || cachePath.back() != '/' ? "/" : "") + FILE_NAME + suffix;
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
  for (const auto& path : paths) {
    PerBookReaderSettings ignored;
    const ReadStatus status = readStored(path, ignored);
    if (status == ReadStatus::NEWER_VERSION || status == ReadStatus::IO_ERROR) return false;
  }
  bool success = true;
  for (const auto& path : paths) success = removeIfPresent(path) && success;
  return success;
}

}  // namespace PerBookReaderSettingsStore
