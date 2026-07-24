#include "StagedFileTransaction.h"

#include <HalStorage.h>

#include <array>

namespace StagedFileTransaction {
namespace {

bool removeIfPresent(const char* path) { return !Storage.exists(path) || Storage.remove(path); }

struct Digest {
  uint64_t size = 0;
  uint32_t hash = 2166136261U;
};

bool digestFile(const char* path, Digest& digest) {
  HalFile file;
  if (!Storage.openFileForRead("STAGED", path, file)) return false;
  digest = {file.fileSize64(), 2166136261U};
  std::array<uint8_t, 512> buffer{};
  uint64_t remaining = digest.size;
  while (remaining > 0) {
    const size_t chunk = remaining < buffer.size() ? static_cast<size_t>(remaining) : buffer.size();
    if (file.read(buffer.data(), chunk) != static_cast<int>(chunk)) {
      file.close();
      return false;
    }
    for (size_t i = 0; i < chunk; ++i) {
      digest.hash ^= buffer[i];
      digest.hash *= 16777619U;
    }
    remaining -= chunk;
  }
  return file.fileSize64() == digest.size && file.close();
}

}  // namespace

Status recover(const char* finalPath, const char* backupPath, const Validator validator, void* context) {
  if (!finalPath || !backupPath || !validator) return Status::IoError;
  if (!Storage.exists(backupPath)) return Status::NoRecoveryNeeded;

  if (Storage.exists(finalPath) && validator(finalPath, context)) {
    return removeIfPresent(backupPath) ? Status::NoRecoveryNeeded : Status::IoError;
  }
  if (Storage.exists(finalPath) && !Storage.remove(finalPath)) return Status::IoError;
  if (!Storage.rename(backupPath, finalPath)) return Status::IoError;
  return validator(finalPath, context) ? Status::Recovered : Status::IoError;
}

Status publish(const char* finalPath, const char* stagingPath, const char* backupPath, const Validator validator,
               void* context) {
  if (!finalPath || !stagingPath || !backupPath || !validator || !Storage.exists(stagingPath) ||
      !validator(stagingPath, context)) {
    return Status::InvalidStaging;
  }
  Digest expected;
  if (!digestFile(stagingPath, expected)) return Status::IoError;

  const Status recovered = recover(finalPath, backupPath, validator, context);
  if (recovered == Status::IoError) return Status::IoError;

  bool rotated = false;
  if (Storage.exists(finalPath)) {
    if (!removeIfPresent(backupPath) || !Storage.rename(finalPath, backupPath)) return Status::IoError;
    rotated = true;
  }

  if (!Storage.rename(stagingPath, finalPath)) {
    if (rotated && !Storage.exists(finalPath)) Storage.rename(backupPath, finalPath);
    return Status::IoError;
  }
  Digest published;
  if (!validator(finalPath, context) || !digestFile(finalPath, published) || published.size != expected.size ||
      published.hash != expected.hash) {
    removeIfPresent(finalPath);
    if (rotated) Storage.rename(backupPath, finalPath);
    return Status::IoError;
  }

  if (rotated) removeIfPresent(backupPath);
  return Status::Published;
}

}  // namespace StagedFileTransaction
