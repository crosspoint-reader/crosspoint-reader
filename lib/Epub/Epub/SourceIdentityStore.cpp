#include "SourceIdentityStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <string>

#include "SourceIdentityCodec.h"

namespace SourceIdentityStore {
namespace {

enum class CandidateStatus : uint8_t { Valid, Missing, NewerVersion, Invalid, IoError };

struct Candidate {
  CandidateStatus status = CandidateStatus::Missing;
  ZipFile::SourceIdentity identity{};
};

std::string pathFor(const std::string& cachePath, const char* suffix) {
  return cachePath + (cachePath.empty() || cachePath.back() == '/' ? "" : "/") + FILE_NAME + suffix;
}

Candidate inspect(const std::string& path) {
  if (!Storage.exists(path.c_str())) return {};
  HalFile file;
  if (!Storage.openFileForRead("SID", path, file)) return {CandidateStatus::IoError, {}};

  const size_t size = file.fileSize();
  if (size > SourceIdentityCodec::ENCODED_SIZE) {
    std::array<uint8_t, SourceIdentityCodec::VERSION_OFFSET + 1> prefix{};
    const bool read = file.read(prefix.data(), prefix.size()) == static_cast<int>(prefix.size());
    const bool closed = file.close();
    if (!read || !closed) return {CandidateStatus::IoError, {}};
    if (std::equal(SourceIdentityCodec::MAGIC.begin(), SourceIdentityCodec::MAGIC.end(), prefix.begin()) &&
        prefix[SourceIdentityCodec::VERSION_OFFSET] > SourceIdentityCodec::VERSION) {
      return {CandidateStatus::NewerVersion, {}};
    }
    return {CandidateStatus::Invalid, {}};
  }

  SourceIdentityCodec::Encoded encoded{};
  const bool read = file.read(encoded.data(), size) == static_cast<int>(size);
  const bool closed = file.close();
  if (!read || !closed) return {CandidateStatus::IoError, {}};

  ZipFile::SourceIdentity identity;
  const SourceIdentityCodec::DecodeStatus decoded = SourceIdentityCodec::decode(encoded.data(), size, identity);
  if (decoded == SourceIdentityCodec::DecodeStatus::OK) return {CandidateStatus::Valid, identity};
  if (decoded == SourceIdentityCodec::DecodeStatus::NEWER_VERSION) {
    return {CandidateStatus::NewerVersion, {}};
  }
  return {CandidateStatus::Invalid, {}};
}

bool removeIfPresent(const std::string& path) { return !Storage.exists(path.c_str()) || Storage.remove(path.c_str()); }

bool writeVerified(const std::string& path, const SourceIdentityCodec::Encoded& encoded,
                   const ZipFile::SourceIdentity& expected) {
  HalFile file;
  if (!Storage.openFileForWrite("SID", path, file)) return false;
  if (file.write(encoded.data(), encoded.size()) != encoded.size()) {
    file.close();
    return false;
  }
  file.flush();
  const bool synced = file.sync();
  const bool closed = file.close();
  if (!synced || !closed) return false;
  const Candidate verified = inspect(path);
  return verified.status == CandidateStatus::Valid && verified.identity == expected;
}

ZipFile::SourceIdentity replacementBarrier() {
  // No valid ZIP can have a one-byte central directory containing an entry.
  return {22, 0, 1, 1, 0};
}

bool isUsableLoadStatus(const LoadStatus status) {
  return status == LoadStatus::Primary || status == LoadStatus::Backup || status == LoadStatus::Temp;
}

}  // namespace

LoadStatus load(const std::string& cachePath, ZipFile::SourceIdentity& identity) {
  const std::array<std::string, 3> paths = {pathFor(cachePath, ""), pathFor(cachePath, ".bak"),
                                            pathFor(cachePath, ".tmp")};
  constexpr std::array<LoadStatus, 3> sources = {LoadStatus::Primary, LoadStatus::Backup, LoadStatus::Temp};
  bool invalid = false;
  for (size_t i = 0; i < paths.size(); ++i) {
    const Candidate candidate = inspect(paths[i]);
    if (candidate.status == CandidateStatus::Valid) {
      identity = candidate.identity;
      return sources[i];
    }
    if (candidate.status == CandidateStatus::NewerVersion) return LoadStatus::NewerVersion;
    if (candidate.status == CandidateStatus::IoError) return LoadStatus::IoError;
    invalid = invalid || candidate.status == CandidateStatus::Invalid;
  }
  return invalid ? LoadStatus::Invalid : LoadStatus::Missing;
}

SaveStatus save(const std::string& cachePath, const ZipFile::SourceIdentity& identity) {
  SourceIdentityCodec::Encoded encoded;
  if (!SourceIdentityCodec::encode(identity, encoded)) return SaveStatus::IoError;

  if (!Storage.exists(cachePath.c_str()) && !Storage.mkdir(cachePath.c_str())) return SaveStatus::IoError;

  const std::string primaryPath = pathFor(cachePath, "");
  const std::string backupPath = pathFor(cachePath, ".bak");
  const std::string tempPath = pathFor(cachePath, ".tmp");
  const std::array<std::string, 3> paths = {primaryPath, backupPath, tempPath};
  std::array<Candidate, 3> candidates;
  for (size_t i = 0; i < paths.size(); ++i) {
    candidates[i] = inspect(paths[i]);
    if (candidates[i].status == CandidateStatus::NewerVersion) return SaveStatus::NewerVersion;
    if (candidates[i].status == CandidateStatus::IoError) return SaveStatus::IoError;
  }
  if (candidates[0].status == CandidateStatus::Valid && candidates[0].identity == identity) {
    return SaveStatus::Unchanged;
  }

  if (!removeIfPresent(tempPath) || !writeVerified(tempPath, encoded, identity)) {
    removeIfPresent(tempPath);
    return SaveStatus::IoError;
  }

  const bool primaryWasValid = candidates[0].status == CandidateStatus::Valid;
  bool rotated = false;
  if (primaryWasValid) {
    if (!removeIfPresent(backupPath) || !Storage.rename(primaryPath.c_str(), backupPath.c_str())) {
      removeIfPresent(tempPath);
      return SaveStatus::IoError;
    }
    rotated = true;
  } else if (!removeIfPresent(primaryPath)) {
    removeIfPresent(tempPath);
    return SaveStatus::IoError;
  }

  if (!Storage.rename(tempPath.c_str(), primaryPath.c_str())) {
    if (rotated && !Storage.exists(primaryPath.c_str())) Storage.rename(backupPath.c_str(), primaryPath.c_str());
    return SaveStatus::IoError;
  }
  const Candidate published = inspect(primaryPath);
  if (published.status != CandidateStatus::Valid || published.identity != identity) {
    // Keep a verified fallback when the first publication was damaged.
    if (!rotated) writeVerified(tempPath, encoded, identity);
    return SaveStatus::IoError;
  }
  return SaveStatus::Saved;
}

bool isReplacementBarrier(const ZipFile::SourceIdentity& identity) { return identity == replacementBarrier(); }

PrepareReplacementStatus prepareReplacement(const std::string& cachePath,
                                            const ZipFile::SourceIdentity* currentIdentity) {
  ZipFile::SourceIdentity stored;
  LoadStatus loaded = load(cachePath, stored);
  if (loaded == LoadStatus::NewerVersion) return PrepareReplacementStatus::NewerVersion;
  if (loaded == LoadStatus::Invalid) return PrepareReplacementStatus::Invalid;
  if (loaded == LoadStatus::IoError) return PrepareReplacementStatus::IoError;

  if (loaded == LoadStatus::Primary && isReplacementBarrier(stored)) {
    const Candidate fallback = inspect(pathFor(cachePath, ".bak"));
    const Candidate unpublished = inspect(pathFor(cachePath, ".tmp"));
    if (fallback.status == CandidateStatus::NewerVersion) return PrepareReplacementStatus::NewerVersion;
    if (unpublished.status == CandidateStatus::NewerVersion) return PrepareReplacementStatus::NewerVersion;
    if (fallback.status == CandidateStatus::IoError || unpublished.status == CandidateStatus::IoError) {
      return PrepareReplacementStatus::IoError;
    }
    if (fallback.status == CandidateStatus::Invalid || unpublished.status == CandidateStatus::Invalid ||
        (unpublished.status == CandidateStatus::Valid &&
         (fallback.status != CandidateStatus::Valid || unpublished.identity != fallback.identity))) {
      return PrepareReplacementStatus::Invalid;
    }
    if (currentIdentity && (fallback.status != CandidateStatus::Valid || fallback.identity != *currentIdentity)) {
      return PrepareReplacementStatus::Invalid;
    }
    return PrepareReplacementStatus::Prepared;
  }

  if (currentIdentity) {
    if (isUsableLoadStatus(loaded) && stored != *currentIdentity) return PrepareReplacementStatus::Invalid;

    // Legacy state may not have a sidecar yet, while a recovered backup/temp
    // may not be primary. Normalize the actual current source first so save()
    // can rotate it into a durable backup when publishing the barrier.
    if (loaded != LoadStatus::Primary) {
      const SaveStatus normalized = save(cachePath, *currentIdentity);
      if (normalized == SaveStatus::NewerVersion) return PrepareReplacementStatus::NewerVersion;
      if (normalized == SaveStatus::IoError) return PrepareReplacementStatus::IoError;
    }
  } else if (isUsableLoadStatus(loaded)) {
    // No backing file exists, but old path-keyed state may still carry an
    // identity. Preserve that identity as the fallback rather than discarding
    // evidence about ownership.
    if (loaded != LoadStatus::Primary) {
      const SaveStatus normalized = save(cachePath, stored);
      if (normalized == SaveStatus::NewerVersion) return PrepareReplacementStatus::NewerVersion;
      if (normalized == SaveStatus::IoError) return PrepareReplacementStatus::IoError;
    }
  }

  const ZipFile::SourceIdentity barrier = replacementBarrier();
  const SaveStatus prepared = save(cachePath, barrier);
  if (prepared == SaveStatus::NewerVersion) return PrepareReplacementStatus::NewerVersion;
  if (prepared == SaveStatus::IoError) return PrepareReplacementStatus::IoError;

  const Candidate primary = inspect(pathFor(cachePath, ""));
  if (primary.status != CandidateStatus::Valid || !isReplacementBarrier(primary.identity)) {
    return PrepareReplacementStatus::IoError;
  }
  if (currentIdentity) {
    const Candidate backup = inspect(pathFor(cachePath, ".bak"));
    if (backup.status != CandidateStatus::Valid || backup.identity != *currentIdentity) {
      return PrepareReplacementStatus::IoError;
    }
  }
  return PrepareReplacementStatus::Prepared;
}

bool cancelReplacement(const std::string& cachePath) {
  const std::string primaryPath = pathFor(cachePath, "");
  const std::string backupPath = pathFor(cachePath, ".bak");
  const std::string tempPath = pathFor(cachePath, ".tmp");
  const Candidate primary = inspect(primaryPath);
  if (primary.status == CandidateStatus::Missing) return true;
  if (primary.status != CandidateStatus::Valid || !isReplacementBarrier(primary.identity)) return false;

  const Candidate backup = inspect(backupPath);
  const Candidate temp = inspect(tempPath);
  if (backup.status == CandidateStatus::NewerVersion || temp.status == CandidateStatus::NewerVersion ||
      backup.status == CandidateStatus::IoError || temp.status == CandidateStatus::IoError ||
      backup.status == CandidateStatus::Invalid || temp.status == CandidateStatus::Invalid ||
      (temp.status == CandidateStatus::Valid &&
       (backup.status != CandidateStatus::Valid || temp.identity != backup.identity))) {
    return false;
  }
  if (backup.status == CandidateStatus::Missing) {
    return removeIfPresent(tempPath) && removeIfPresent(primaryPath) &&
           inspect(primaryPath).status == CandidateStatus::Missing;
  }

  SourceIdentityCodec::Encoded encoded;
  if (!SourceIdentityCodec::encode(backup.identity, encoded) || !removeIfPresent(tempPath) ||
      !writeVerified(tempPath, encoded, backup.identity) || !removeIfPresent(primaryPath) ||
      !Storage.rename(tempPath.c_str(), primaryPath.c_str())) {
    return false;
  }
  const Candidate restored = inspect(primaryPath);
  return restored.status == CandidateStatus::Valid && restored.identity == backup.identity;
}

RecoverReplacementStatus recoverReplacement(const std::string& cachePath,
                                            const ZipFile::SourceIdentity& currentIdentity) {
  const Candidate primary = inspect(pathFor(cachePath, ""));
  if (primary.status == CandidateStatus::Missing) {
    // save() may have rotated the retained identity before power was lost
    // publishing the barrier. Do not treat an unrecognized sibling as an
    // ordinary absence: the caller could otherwise discard the physical book
    // backup before the replacement state is resolved.
    const Candidate backup = inspect(pathFor(cachePath, ".bak"));
    const Candidate temp = inspect(pathFor(cachePath, ".tmp"));
    if (backup.status == CandidateStatus::NewerVersion || temp.status == CandidateStatus::NewerVersion) {
      return RecoverReplacementStatus::NewerVersion;
    }
    if (backup.status == CandidateStatus::IoError || temp.status == CandidateStatus::IoError) {
      return RecoverReplacementStatus::IoError;
    }
    if (backup.status == CandidateStatus::Invalid || temp.status == CandidateStatus::Invalid ||
        (backup.status == CandidateStatus::Valid && temp.status == CandidateStatus::Valid &&
         backup.identity != temp.identity && !isReplacementBarrier(temp.identity))) {
      return RecoverReplacementStatus::Invalid;
    }

    const Candidate* retained =
        backup.status == CandidateStatus::Valid ? &backup : (temp.status == CandidateStatus::Valid ? &temp : nullptr);
    if (!retained) return RecoverReplacementStatus::NotPrepared;
    if (retained->identity != currentIdentity) return RecoverReplacementStatus::ReplacementPublished;

    const SaveStatus restored = save(cachePath, currentIdentity);
    if (restored == SaveStatus::NewerVersion) return RecoverReplacementStatus::NewerVersion;
    return restored == SaveStatus::Saved || restored == SaveStatus::Unchanged
               ? RecoverReplacementStatus::RestoredCurrentSource
               : RecoverReplacementStatus::IoError;
  }
  if (primary.status == CandidateStatus::NewerVersion) return RecoverReplacementStatus::NewerVersion;
  if (primary.status == CandidateStatus::IoError) return RecoverReplacementStatus::IoError;
  if (primary.status == CandidateStatus::Invalid) return RecoverReplacementStatus::Invalid;
  if (!isReplacementBarrier(primary.identity)) return RecoverReplacementStatus::NotPrepared;

  const Candidate backup = inspect(pathFor(cachePath, ".bak"));
  const Candidate temp = inspect(pathFor(cachePath, ".tmp"));
  if (backup.status == CandidateStatus::NewerVersion || temp.status == CandidateStatus::NewerVersion) {
    return RecoverReplacementStatus::NewerVersion;
  }
  if (backup.status == CandidateStatus::IoError || temp.status == CandidateStatus::IoError) {
    return RecoverReplacementStatus::IoError;
  }
  if (backup.status == CandidateStatus::Invalid || temp.status == CandidateStatus::Invalid ||
      (temp.status == CandidateStatus::Valid &&
       (backup.status != CandidateStatus::Valid || temp.identity != backup.identity))) {
    return RecoverReplacementStatus::Invalid;
  }
  if (backup.status == CandidateStatus::Valid && backup.identity == currentIdentity) {
    return cancelReplacement(cachePath) ? RecoverReplacementStatus::RestoredCurrentSource
                                        : RecoverReplacementStatus::IoError;
  }
  return RecoverReplacementStatus::ReplacementPublished;
}

}  // namespace SourceIdentityStore
