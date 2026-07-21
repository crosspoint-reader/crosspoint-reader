#pragma once

#include <ZipFile.h>

#include <cstdint>
#include <string>

namespace SourceIdentityStore {

constexpr char FILE_NAME[] = "source_identity.bin";

enum class LoadStatus : uint8_t { Primary, Backup, Temp, Missing, NewerVersion, Invalid, IoError };
enum class SaveStatus : uint8_t { Saved, Unchanged, NewerVersion, IoError };
enum class PrepareReplacementStatus : uint8_t { Prepared, NewerVersion, Invalid, IoError };
enum class RecoverReplacementStatus : uint8_t {
  NotPrepared,
  RestoredCurrentSource,
  ReplacementPublished,
  NewerVersion,
  Invalid,
  IoError,
};

LoadStatus load(const std::string& cachePath, ZipFile::SourceIdentity& identity);
SaveStatus save(const std::string& cachePath, const ZipFile::SourceIdentity& identity);

// Publishes a durable fail-closed marker while retaining the prior identity as
// a verified fallback. currentIdentity must identify the still-authoritative
// book when one exists; pass nullptr only when no book currently occupies the
// path. This must complete before that path is overwritten.
PrepareReplacementStatus prepareReplacement(const std::string& cachePath,
                                            const ZipFile::SourceIdentity* currentIdentity);

// If the marker is still present and currentIdentity is the retained old
// source, restore it as primary (the upload never published). Otherwise report
// that the replacement reached the final path and must be quarantined.
RecoverReplacementStatus recoverReplacement(const std::string& cachePath,
                                            const ZipFile::SourceIdentity& currentIdentity);

// Roll back a prepared marker after publication failed while the old source is
// still authoritative (or no source occupied the path).
bool cancelReplacement(const std::string& cachePath);
bool isReplacementBarrier(const ZipFile::SourceIdentity& identity);

}  // namespace SourceIdentityStore
