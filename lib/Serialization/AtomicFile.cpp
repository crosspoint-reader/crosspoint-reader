#include "AtomicFile.h"

#include <HalStorage.h>

#include <array>
#include <utility>

namespace AtomicFile {
namespace {

enum class CandidateStatus : uint8_t { Valid, Missing, Invalid, Oversize, IoError };

struct Candidate {
  CandidateStatus status = CandidateStatus::Missing;
  std::string data;
};

std::string siblingPath(const char* path, const char* suffix) { return std::string(path) + suffix; }

Candidate inspect(const std::string& path, const size_t maxSize, const Validator validator, void* context) {
  if (!Storage.exists(path.c_str())) return {};

  HalFile file;
  if (!Storage.openFileForRead("ATOMIC", path, file)) return {CandidateStatus::IoError, {}};
  const uint64_t fileSize = file.fileSize64();
  if (fileSize > maxSize || fileSize > static_cast<uint64_t>(SIZE_MAX)) {
    if (!file.close()) return {CandidateStatus::IoError, {}};
    return {CandidateStatus::Oversize, {}};
  }

  Candidate candidate;
  candidate.data.resize(static_cast<size_t>(fileSize));
  const bool read = fileSize == 0 ||
                    file.read(candidate.data.data(), static_cast<size_t>(fileSize)) == static_cast<int>(fileSize);
  const bool stable = file.fileSize64() == fileSize;
  const bool closed = file.close();
  if (!read || !stable || !closed) return {CandidateStatus::IoError, {}};
  if (!validator(reinterpret_cast<const uint8_t*>(candidate.data.data()), candidate.data.size(), context)) {
    candidate.data.clear();
    candidate.status = CandidateStatus::Invalid;
    return candidate;
  }
  candidate.status = CandidateStatus::Valid;
  return candidate;
}

bool removeIfPresent(const std::string& path) { return !Storage.exists(path.c_str()) || Storage.remove(path.c_str()); }

bool writeVerified(const std::string& path, const uint8_t* data, const size_t size, const size_t maxSize,
                   const Validator validator, void* context) {
  HalFile file;
  if (!Storage.openFileForWrite("ATOMIC", path, file)) return false;
  const bool written = size == 0 || file.write(data, size) == size;
  file.flush();
  const bool synced = written && file.sync();
  const bool closed = file.close();
  if (!written || !synced || !closed) return false;
  const Candidate verified = inspect(path, maxSize, validator, context);
  return verified.status == CandidateStatus::Valid && verified.data.size() == size &&
         (size == 0 || verified.data.compare(0, size, reinterpret_cast<const char*>(data), size) == 0);
}

}  // namespace

LoadStatus load(const char* path, std::string& data, const size_t maxSize, const Validator validator, void* context) {
  data.clear();
  if (!path || !validator) return LoadStatus::Invalid;

  const std::array<std::string, 3> paths = {path, siblingPath(path, ".bak"), siblingPath(path, ".tmp")};
  constexpr std::array<LoadStatus, 3> sources = {LoadStatus::Primary, LoadStatus::Backup, LoadStatus::Temp};
  bool invalid = false;
  bool oversize = false;
  for (size_t i = 0; i < paths.size(); ++i) {
    Candidate candidate = inspect(paths[i], maxSize, validator, context);
    if (candidate.status == CandidateStatus::Valid) {
      data = std::move(candidate.data);
      return sources[i];
    }
    if (candidate.status == CandidateStatus::IoError) return LoadStatus::IoError;
    invalid = invalid || candidate.status == CandidateStatus::Invalid;
    oversize = oversize || candidate.status == CandidateStatus::Oversize;
  }
  if (oversize) return LoadStatus::Oversize;
  return invalid ? LoadStatus::Invalid : LoadStatus::Missing;
}

SaveStatus save(const char* path, const uint8_t* data, const size_t size, const size_t maxSize,
                const Validator validator, void* context) {
  if (!path || !validator || (!data && size != 0)) return SaveStatus::InvalidExistingState;
  if (size > maxSize) return SaveStatus::Oversize;
  if (!validator(data, size, context)) return SaveStatus::InvalidExistingState;

  const std::string primaryPath(path);
  const std::string backupPath = siblingPath(path, ".bak");
  const std::string tempPath = siblingPath(path, ".tmp");
  Candidate primary = inspect(primaryPath, maxSize, validator, context);
  Candidate backup = inspect(backupPath, maxSize, validator, context);
  if (primary.status == CandidateStatus::IoError || backup.status == CandidateStatus::IoError) {
    return SaveStatus::IoError;
  }
  if (primary.status == CandidateStatus::Valid && primary.data.size() == size &&
      (size == 0 || primary.data.compare(0, size, reinterpret_cast<const char*>(data), size) == 0)) {
    return SaveStatus::Unchanged;
  }

  if (!removeIfPresent(tempPath) || !writeVerified(tempPath, data, size, maxSize, validator, context)) {
    removeIfPresent(tempPath);
    return SaveStatus::IoError;
  }

  bool rotated = false;
  if (primary.status == CandidateStatus::Valid) {
    if (!removeIfPresent(backupPath) || !Storage.rename(primaryPath.c_str(), backupPath.c_str())) {
      removeIfPresent(tempPath);
      return SaveStatus::IoError;
    }
    rotated = true;
  } else {
    // With no valid primary, retain an already-valid backup or create a
    // verified copy of the new value before touching the primary path.
    if (backup.status != CandidateStatus::Valid) {
      if (!removeIfPresent(backupPath) || !writeVerified(backupPath, data, size, maxSize, validator, context)) {
        removeIfPresent(tempPath);
        return SaveStatus::IoError;
      }
    }
    if (!removeIfPresent(primaryPath)) {
      removeIfPresent(tempPath);
      return SaveStatus::IoError;
    }
  }

  if (!Storage.rename(tempPath.c_str(), primaryPath.c_str())) {
    if (rotated && !Storage.exists(primaryPath.c_str())) Storage.rename(backupPath.c_str(), primaryPath.c_str());
    return SaveStatus::IoError;
  }

  const Candidate published = inspect(primaryPath, maxSize, validator, context);
  if (published.status != CandidateStatus::Valid || published.data.size() != size ||
      (size != 0 && published.data.compare(0, size, reinterpret_cast<const char*>(data), size) != 0)) {
    // The valid backup is deliberately retained. Restore is best effort; a
    // failed restore still leaves a recoverable sibling for load().
    if (Storage.exists(primaryPath.c_str())) Storage.remove(primaryPath.c_str());
    if (Storage.exists(backupPath.c_str())) Storage.rename(backupPath.c_str(), primaryPath.c_str());
    return SaveStatus::IoError;
  }
  return SaveStatus::Saved;
}

}  // namespace AtomicFile
