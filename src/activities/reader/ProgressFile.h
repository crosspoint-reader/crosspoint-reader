#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace ProgressFile {

enum class LoadSource : uint8_t { Missing, Primary, Backup, Temp, Invalid, IoError };

struct LoadResult {
  LoadSource source = LoadSource::Missing;
  size_t size = 0;

  explicit operator bool() const {
    return source == LoadSource::Primary || source == LoadSource::Backup || source == LoadSource::Temp;
  }
};

using ValidatorFunction = bool (*)(const uint8_t* data, size_t size, const void* context);

// Optional semantic validation applied independently to primary, backup and
// temp candidates. Size checks alone cannot distinguish a complete but
// nonsensical progress record from a usable one.
struct CandidateValidator {
  ValidatorFunction function = nullptr;
  const void* context = nullptr;

  bool accepts(const uint8_t* data, const size_t size) const {
    return function == nullptr || function(data, size, context);
  }
};

struct EpubBounds {
  uint32_t spineCount = 0;
};

struct PageBounds {
  uint32_t pageCount = 0;
};

inline bool validateEpubBounds(const uint8_t* data, const size_t size, const void* context) {
  if (!data || (size != 4 && size != 6) || !context) return false;
  const auto& bounds = *static_cast<const EpubBounds*>(context);
  if (bounds.spineCount == 0) return false;

  const uint16_t spineIndex = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
  const uint16_t pageNumber = static_cast<uint16_t>(data[2]) | (static_cast<uint16_t>(data[3]) << 8);
  if (spineIndex >= bounds.spineCount || pageNumber == UINT16_MAX) return false;

  if (size == 6) {
    const uint16_t pageCount = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
    // A zero count is intentionally supported for legacy/footnote save paths
    // that know the exact resume page but do not yet know the chapter total.
    if (pageCount > 0 && pageNumber >= pageCount) return false;
  }
  return true;
}

inline bool validatePageBounds(const uint8_t* data, const size_t size, const void* context) {
  if (!data || size != 4 || !context) return false;
  const auto& bounds = *static_cast<const PageBounds*>(context);
  if (bounds.pageCount == 0) return false;

  const uint32_t page = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                        (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return page < bounds.pageCount;
}

namespace detail {

enum class CandidateStatus : uint8_t { Missing, Valid, Invalid, IoError };

struct CandidateResult {
  CandidateStatus status = CandidateStatus::Missing;
  size_t size = 0;
};

inline bool acceptsSize(const size_t size, const size_t* acceptedSizes, const size_t acceptedSizeCount) {
  for (size_t i = 0; i < acceptedSizeCount; ++i) {
    if (size == acceptedSizes[i]) return true;
  }
  return false;
}

inline CandidateResult readCandidate(const std::string& path, uint8_t* data, const size_t capacity,
                                     const size_t* acceptedSizes, const size_t acceptedSizeCount,
                                     const CandidateValidator validator = {}) {
  if (!Storage.exists(path.c_str())) return {};

  HalFile file;
  if (!Storage.openFileForRead("PRG", path, file)) return {CandidateStatus::IoError, 0};

  const size_t size = file.fileSize();
  if (!acceptsSize(size, acceptedSizes, acceptedSizeCount) || size > capacity) {
    return {file.close() ? CandidateStatus::Invalid : CandidateStatus::IoError, size};
  }
  if (file.read(data, size) != static_cast<int>(size)) {
    file.close();
    return {CandidateStatus::IoError, size};
  }
  if (!file.close()) return {CandidateStatus::IoError, size};
  return {validator.accepts(data, size) ? CandidateStatus::Valid : CandidateStatus::Invalid, size};
}

inline LoadResult load(const std::string& cachePath, uint8_t* data, const size_t capacity, const size_t* acceptedSizes,
                       const size_t acceptedSizeCount, const CandidateValidator validator = {}) {
  if (!data || capacity == 0 || !acceptedSizes || acceptedSizeCount == 0) return {LoadSource::Invalid, 0};

  const std::string primaryPath = cachePath + "/progress.bin";
  const std::string backupPath = primaryPath + ".bak";
  const std::string tempPath = primaryPath + ".tmp";
  const std::string* paths[] = {&primaryPath, &backupPath, &tempPath};
  constexpr LoadSource sources[] = {LoadSource::Primary, LoadSource::Backup, LoadSource::Temp};
  bool sawInvalid = false;
  bool sawIoError = false;

  for (size_t i = 0; i < 3; ++i) {
    const CandidateResult candidate =
        readCandidate(*paths[i], data, capacity, acceptedSizes, acceptedSizeCount, validator);
    if (candidate.status == CandidateStatus::Valid) return {sources[i], candidate.size};
    sawInvalid = sawInvalid || candidate.status == CandidateStatus::Invalid;
    sawIoError = sawIoError || candidate.status == CandidateStatus::IoError;
  }
  if (sawIoError) return {LoadSource::IoError, 0};
  return {sawInvalid ? LoadSource::Invalid : LoadSource::Missing, 0};
}

inline bool verifyExact(const std::string& path, const uint8_t* expected, const size_t size) {
  uint8_t actual[6]{};
  const size_t acceptedSize = size;
  const CandidateResult result = readCandidate(path, actual, sizeof(actual), &acceptedSize, 1);
  return result.status == CandidateStatus::Valid && memcmp(actual, expected, size) == 0;
}

inline bool writeVerified(const std::string& path, const uint8_t* data, const size_t size) {
  HalFile file;
  if (!Storage.openFileForWrite("PRG", path, file)) return false;
  if (file.write(data, size) != size) {
    file.close();
    return false;
  }
  file.flush();
  const bool synced = file.sync();
  const bool closed = file.close();
  return synced && closed && verifyExact(path, data, size);
}

inline bool removeIfPresent(const std::string& path) {
  return !Storage.exists(path.c_str()) || Storage.remove(path.c_str());
}

}  // namespace detail

// EPUB progress is six bytes today and four bytes in the legacy layout. Both
// are intentionally accepted without changing either on-disk format.
inline LoadResult loadEpub(const std::string& cachePath, uint8_t* data, const size_t capacity,
                           const CandidateValidator validator = {}) {
  constexpr size_t ACCEPTED_SIZES[] = {6, 4};
  return detail::load(cachePath, data, capacity, ACCEPTED_SIZES, 2, validator);
}

// TXT and XTC share the original four-byte little-endian page layout.
inline LoadResult loadPage(const std::string& cachePath, uint8_t* data, const size_t capacity,
                           const CandidateValidator validator = {}) {
  constexpr size_t ACCEPTED_SIZE = 4;
  return detail::load(cachePath, data, capacity, &ACCEPTED_SIZE, 1, validator);
}

// Writes the existing four- or six-byte layout through progress.bin.tmp,
// keeps the previous committed file as progress.bin.bak, and verifies bytes
// before and after publication. Readers fall back to backup and then temp, so
// every interruption point retains at least one usable copy on a healthy FAT.
inline bool writeAtomic(const std::string& cachePath, const uint8_t* data, const size_t len,
                        const CandidateValidator validator = {}) {
  if (!data || (len != 4 && len != 6) || !validator.accepts(data, len)) return false;

  const std::string primaryPath = cachePath + "/progress.bin";
  const std::string backupPath = primaryPath + ".bak";
  const std::string tempPath = primaryPath + ".tmp";
  const size_t acceptedSizes[] = {len, 4};
  const size_t acceptedSizeCount = len == 6 ? 2 : 1;
  uint8_t scratch[6]{};
  auto primary =
      detail::readCandidate(primaryPath, scratch, sizeof(scratch), acceptedSizes, acceptedSizeCount, validator);
  const auto backup =
      detail::readCandidate(backupPath, scratch, sizeof(scratch), acceptedSizes, acceptedSizeCount, validator);
  const auto temp =
      detail::readCandidate(tempPath, scratch, sizeof(scratch), acceptedSizes, acceptedSizeCount, validator);
  const std::array<detail::CandidateResult, 3> candidates = {primary, backup, temp};

  // An unreadable sibling suggests an SD/FAT problem. A file larger than any
  // known layout may belong to newer firmware. Preserve both cases unchanged.
  const bool hasProtectedSibling = std::any_of(candidates.begin(), candidates.end(), [len](const auto& candidate) {
    return candidate.status == detail::CandidateStatus::IoError ||
           (candidate.status == detail::CandidateStatus::Invalid && candidate.size > len);
  });
  if (hasProtectedSibling) {
    LOG_ERR("PRG", "Refusing to overwrite unreadable or unknown progress state");
    return false;
  }

  // If a first-ever save was fully written but power failed before publication,
  // promote it before reusing the temp path. Otherwise a committed copy already
  // protects us and the old temp is stale.
  if (temp.status == detail::CandidateStatus::Valid && primary.status != detail::CandidateStatus::Valid &&
      backup.status != detail::CandidateStatus::Valid) {
    if ((primary.status == detail::CandidateStatus::Invalid && !Storage.remove(primaryPath.c_str())) ||
        !Storage.rename(tempPath.c_str(), primaryPath.c_str())) {
      LOG_ERR("PRG", "Could not recover the only valid progress copy");
      return false;
    }
    primary = temp;
  } else if (!detail::removeIfPresent(tempPath)) {
    LOG_ERR("PRG", "Could not clear stale temp progress file: %s", tempPath.c_str());
    return false;
  }

  if (!detail::writeVerified(tempPath, data, len)) {
    LOG_ERR("PRG", "Could not fully write, sync, and verify temp progress file: %s", tempPath.c_str());
    detail::removeIfPresent(tempPath);
    return false;
  }

  bool rotated = false;
  if (primary.status == detail::CandidateStatus::Valid) {
    if (!detail::removeIfPresent(backupPath) || !Storage.rename(primaryPath.c_str(), backupPath.c_str())) {
      LOG_ERR("PRG", "Could not rotate progress backup: %s", primaryPath.c_str());
      detail::removeIfPresent(tempPath);
      return false;
    }
    rotated = true;
  } else if (!detail::removeIfPresent(primaryPath)) {
    LOG_ERR("PRG", "Could not replace invalid progress file: %s", primaryPath.c_str());
    detail::removeIfPresent(tempPath);
    return false;
  }

  if (!Storage.rename(tempPath.c_str(), primaryPath.c_str())) {
    LOG_ERR("PRG", "Failed to publish temp progress file: %s", primaryPath.c_str());
    if (rotated && !Storage.rename(backupPath.c_str(), primaryPath.c_str())) {
      LOG_ERR("PRG", "Progress rollback remains available in backup: %s", backupPath.c_str());
    }
    return false;
  }
  if (!detail::verifyExact(primaryPath, data, len)) {
    LOG_ERR("PRG", "Published progress verification failed: %s", primaryPath.c_str());
    // The caller's bytes are still resident. Recreate and verify temp before
    // removing a bad first-ever primary, otherwise this recovery path itself
    // could turn a media error into complete progress loss.
    const bool tempRecovered = detail::writeVerified(tempPath, data, len);
    if (!tempRecovered) LOG_ERR("PRG", "Could not preserve failed publication in temp: %s", tempPath.c_str());
    if (rotated || tempRecovered) {
      if (!detail::removeIfPresent(primaryPath)) {
        LOG_ERR("PRG", "Could not remove failed progress publication: %s", primaryPath.c_str());
      } else if (rotated && !Storage.rename(backupPath.c_str(), primaryPath.c_str())) {
        LOG_ERR("PRG", "Progress rollback remains available in backup: %s", backupPath.c_str());
      }
    }
    return false;
  }
  return true;
}

}  // namespace ProgressFile
