#include "NearbyStatsStorage.h"

#include <array>

#include "NearbyStatsPolicy.h"
#include "activities/reader/ReadingStatsCodec.h"
#include "activities/reader/ReadingStatsEnvelope.h"
#include "activities/reader/ReadingStatsStorage.h"
#include "activities/reader/ReadingStatsVersionGuard.h"

namespace {
enum class FileDecision : uint8_t { Replaceable, Regression, Protected };

FileDecision inspectLegacyFile(const std::string& path, const GlobalReadingStats& incoming) {
  ReadingStatsCodec::GlobalBytes bytes{};
  const ReadingStatsStorage::ReadOutcome read = ReadingStatsStorage::read(path.c_str(), bytes.data(), bytes.size());
  if (ReadingStatsStorage::isProtectedExistingFile(read.result, false)) return FileDecision::Protected;
  if (read.result != ReadingStatsStorage::ReadResult::Ok) return FileDecision::Replaceable;

  GlobalReadingStats decoded;
  const ReadingStatsDecodeResult result = ReadingStatsCodec::decode(bytes.data(), read.size, decoded);
  if (result == ReadingStatsDecodeResult::NewerFormat) return FileDecision::Protected;
  if (result != ReadingStatsDecodeResult::Ok) return FileDecision::Replaceable;
  return NearbyStatsPolicy::doesNotRegress(incoming, decoded) ? FileDecision::Replaceable : FileDecision::Regression;
}

FileDecision inspectEnvelopeFile(const std::string& path, const GlobalReadingStats& incoming,
                                 bool* supportedFormat = nullptr) {
  if (supportedFormat) *supportedFormat = false;
  ReadingStatsCodec::GlobalBytes bytes{};
  const ReadingStatsEnvelope::ReadOutcome read =
      ReadingStatsEnvelope::read(path.c_str(), ReadingStatsEnvelope::Kind::PeerGlobal, bytes.data(), bytes.size());
  if (read.readResult == ReadingStatsStorage::ReadResult::Missing) return FileDecision::Replaceable;
  if (read.readResult == ReadingStatsStorage::ReadResult::TooLarge ||
      read.readResult == ReadingStatsStorage::ReadResult::IoError ||
      read.decodeResult == ReadingStatsEnvelope::DecodeResult::NewerFormat ||
      read.decodeResult == ReadingStatsEnvelope::DecodeResult::PayloadTooLarge ||
      read.decodeResult == ReadingStatsEnvelope::DecodeResult::WrongKind) {
    return FileDecision::Protected;
  }
  if (read.readResult != ReadingStatsStorage::ReadResult::Ok ||
      read.decodeResult != ReadingStatsEnvelope::DecodeResult::Ok)
    return FileDecision::Replaceable;

  GlobalReadingStats decoded;
  const ReadingStatsDecodeResult result = ReadingStatsCodec::decode(bytes.data(), read.payloadSize, decoded);
  if (result == ReadingStatsDecodeResult::NewerFormat) return FileDecision::Protected;
  if (result != ReadingStatsDecodeResult::Ok) return FileDecision::Replaceable;
  if (supportedFormat) *supportedFormat = true;
  return NearbyStatsPolicy::doesNotRegress(incoming, decoded) ? FileDecision::Replaceable : FileDecision::Regression;
}

ReadingStatsVersionGuard::Result scanForNewerPeerFile(const std::string& canonicalPath) {
  constexpr char CURRENT_SUFFIX[] = "_v4.bin";
  const size_t slash = canonicalPath.find_last_of('/');
  const size_t suffix = canonicalPath.rfind(CURRENT_SUFFIX);
  if (suffix == std::string::npos || suffix <= slash || suffix + sizeof(CURRENT_SUFFIX) - 1 != canonicalPath.size()) {
    return ReadingStatsVersionGuard::Result::IoError;
  }
  const std::string directory = slash == 0 ? "/" : canonicalPath.substr(0, slash);
  const size_t fileNameStart = slash == std::string::npos ? 0 : slash + 1;
  const std::string prefix = canonicalPath.substr(fileNameStart, suffix + 2 - fileNameStart);
  return ReadingStatsVersionGuard::scan(directory.c_str(), prefix.c_str(), 4);
}
}  // namespace

namespace NearbyStatsStorage {

Inspection inspect(const std::string& canonicalPath, const std::string& legacyPath,
                   const GlobalReadingStats& incoming) {
  Inspection inspection;
  if (scanForNewerPeerFile(canonicalPath) != ReadingStatsVersionGuard::Result::NoNewerFile) {
    inspection.protectedStorage = true;
    return inspection;
  }
  const std::array<FileDecision, 6> decisions = {
      inspectEnvelopeFile(canonicalPath, incoming, &inspection.rotateCanonical),
      inspectEnvelopeFile(canonicalPath + ".tmp", incoming),
      inspectEnvelopeFile(canonicalPath + ".bak", incoming),
      inspectLegacyFile(legacyPath, incoming),
      inspectLegacyFile(legacyPath + ".tmp", incoming),
      inspectLegacyFile(legacyPath + ".bak", incoming)};
  for (const FileDecision decision : decisions) {
    inspection.protectedStorage = inspection.protectedStorage || decision == FileDecision::Protected;
    inspection.regresses = inspection.regresses || decision == FileDecision::Regression;
  }
  return inspection;
}

bool saveRawSnapshot(const std::string& canonicalPath, const std::string& legacyPath, const uint8_t* rawPayload,
                     const size_t rawPayloadSize, Inspection* inspection) {
  GlobalReadingStats incoming;
  if (!rawPayload || rawPayloadSize != GlobalReadingStats::CURRENT_FILE_SIZE ||
      ReadingStatsCodec::decode(rawPayload, rawPayloadSize, incoming) != ReadingStatsDecodeResult::Ok) {
    return false;
  }

  const Inspection checked = inspect(canonicalPath, legacyPath, incoming);
  if (inspection) *inspection = checked;
  if (checked.protectedStorage || checked.regresses) return false;
  const std::string backupPath = canonicalPath + ".bak";
  return ReadingStatsEnvelope::writeAtomic(canonicalPath.c_str(), backupPath.c_str(), checked.rotateCanonical,
                                           ReadingStatsEnvelope::Kind::PeerGlobal, rawPayload, rawPayloadSize);
}

}  // namespace NearbyStatsStorage
