#pragma once

#include <Epub.h>

#include <cstdint>
#include <string>

namespace SourceIdentityStore {

enum class LoadStatus : uint8_t { Primary, Backup, Temp, Missing, NewerVersion, Invalid, IoError };
enum class PrepareReplacementStatus : uint8_t { Prepared, NewerVersion, Invalid, IoError };
enum class RecoverReplacementStatus : uint8_t {
  NotPrepared,
  RestoredCurrentSource,
  ReplacementPublished,
  NewerVersion,
  Invalid,
  IoError,
};

inline LoadStatus load(const std::string&, ZipFile::SourceIdentity&) { return LoadStatus::Missing; }
inline PrepareReplacementStatus prepareReplacement(const std::string&, const ZipFile::SourceIdentity*) {
  return PrepareReplacementStatus::Prepared;
}
inline RecoverReplacementStatus recoverReplacement(const std::string&, const ZipFile::SourceIdentity&) {
  return RecoverReplacementStatus::NotPrepared;
}
inline bool cancelReplacement(const std::string&) { return true; }

}  // namespace SourceIdentityStore
