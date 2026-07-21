#include "ClippingStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>

namespace {

constexpr std::array<uint8_t, 4> REKEY_MAGIC = {'C', 'V', 'C', 'R'};
constexpr uint8_t REKEY_VERSION_V1 = '1';
constexpr uint8_t REKEY_VERSION_V2 = '2';
constexpr size_t REKEY_PATH_LIMIT = 512;
constexpr size_t REKEY_BASE_HEADER_SIZE = REKEY_MAGIC.size() + 1 + 8;
constexpr size_t REKEY_V2_HEADER_SIZE = REKEY_BASE_HEADER_SIZE + sizeof(uint32_t);

struct RekeyIdentity {
  std::string sourceBook;
  std::string destinationBook;
  std::string sourceStore;
  std::string destinationStore;
};

bool operator==(const RekeyIdentity& left, const RekeyIdentity& right) {
  return left.sourceBook == right.sourceBook && left.destinationBook == right.destinationBook &&
         left.sourceStore == right.sourceStore && left.destinationStore == right.destinationStore;
}

bool validRekeyIdentity(const RekeyIdentity& identity, const std::string_view bookType) {
  const std::array<const std::string*, 4> paths = {&identity.sourceBook, &identity.destinationBook,
                                                   &identity.sourceStore, &identity.destinationStore};
  const bool pathsValid = std::all_of(paths.begin(), paths.end(), [](const std::string* path) {
    return !path->empty() && path->size() <= REKEY_PATH_LIMIT && ClippingCodec::isValidUtf8(*path);
  });
  if (!pathsValid) return false;
  if (identity.sourceBook == identity.destinationBook || identity.sourceStore == identity.destinationStore) {
    return false;
  }
  const std::string expectedSource = ClippingCodec::filePathForBook(identity.sourceBook, bookType);
  const std::string expectedDestination = ClippingCodec::filePathForBook(identity.destinationBook, bookType);
  return !expectedSource.empty() && !expectedDestination.empty() && identity.sourceStore == expectedSource &&
         identity.destinationStore == expectedDestination;
}

std::vector<uint8_t> encodeRekeyIdentity(const RekeyIdentity& identity, const std::string_view bookType) {
  if (!validRekeyIdentity(identity, bookType)) return {};
  const std::array<const std::string*, 4> paths = {&identity.sourceBook, &identity.destinationBook,
                                                   &identity.sourceStore, &identity.destinationStore};
  const size_t size = std::accumulate(paths.begin(), paths.end(), REKEY_V2_HEADER_SIZE,
                                      [](const size_t total, const std::string* path) { return total + path->size(); });
  std::vector<uint8_t> encoded(size);
  std::copy(REKEY_MAGIC.begin(), REKEY_MAGIC.end(), encoded.begin());
  encoded[REKEY_MAGIC.size()] = REKEY_VERSION_V2;
  size_t header = REKEY_MAGIC.size() + 1;
  size_t payload = REKEY_V2_HEADER_SIZE;
  for (const std::string* path : paths) {
    const uint16_t length = static_cast<uint16_t>(path->size());
    encoded[header++] = static_cast<uint8_t>(length & 0xFFU);
    encoded[header++] = static_cast<uint8_t>(length >> 8U);
    std::copy(path->begin(), path->end(), encoded.begin() + static_cast<std::ptrdiff_t>(payload));
    payload += path->size();
  }
  uint32_t checksum = ClippingCodec::crc32(encoded.data(), REKEY_BASE_HEADER_SIZE);
  checksum = ClippingCodec::crc32(encoded.data() + REKEY_V2_HEADER_SIZE, size - REKEY_V2_HEADER_SIZE, checksum);
  for (size_t i = 0; i < sizeof(checksum); ++i) {
    encoded[REKEY_BASE_HEADER_SIZE + i] = static_cast<uint8_t>(checksum >> (i * 8U));
  }
  return encoded;
}

bool readRekeyIdentity(const std::string& markerPath, const std::string_view bookType, RekeyIdentity& identity) {
  HalFile marker;
  if (!Storage.openFileForRead("CLIP", markerPath, marker)) return false;
  const uint64_t size = marker.fileSize64();
  if (size < REKEY_BASE_HEADER_SIZE || size > REKEY_V2_HEADER_SIZE + REKEY_PATH_LIMIT * 4) return false;
  std::array<uint8_t, REKEY_BASE_HEADER_SIZE> header{};
  if (marker.read(header.data(), header.size()) != static_cast<int>(header.size()) ||
      !std::equal(REKEY_MAGIC.begin(), REKEY_MAGIC.end(), header.begin())) {
    return false;
  }
  const uint8_t version = header[REKEY_MAGIC.size()];
  if (version != REKEY_VERSION_V1 && version != REKEY_VERSION_V2) return false;
  const size_t headerSize = version == REKEY_VERSION_V1 ? REKEY_BASE_HEADER_SIZE : REKEY_V2_HEADER_SIZE;
  std::array<uint16_t, 4> lengths{};
  size_t expected = headerSize;
  for (size_t i = 0; i < lengths.size(); ++i) {
    const size_t offset = REKEY_MAGIC.size() + 1 + i * 2;
    lengths[i] = static_cast<uint16_t>(header[offset]) | (static_cast<uint16_t>(header[offset + 1]) << 8U);
    if (lengths[i] == 0 || lengths[i] > REKEY_PATH_LIMIT) return false;
    expected += lengths[i];
  }
  if (size != expected) return false;
  uint32_t storedChecksum = 0;
  if (version == REKEY_VERSION_V2) {
    std::array<uint8_t, sizeof(uint32_t)> checksum{};
    if (marker.read(checksum.data(), checksum.size()) != static_cast<int>(checksum.size())) return false;
    for (size_t i = 0; i < checksum.size(); ++i) {
      storedChecksum |= static_cast<uint32_t>(checksum[i]) << (i * 8U);
    }
  }
  RekeyIdentity parsed;
  std::array<std::string*, 4> paths = {&parsed.sourceBook, &parsed.destinationBook, &parsed.sourceStore,
                                       &parsed.destinationStore};
  for (size_t i = 0; i < paths.size(); ++i) {
    paths[i]->resize(lengths[i]);
    if (marker.read(paths[i]->data(), lengths[i]) != static_cast<int>(lengths[i])) return false;
  }
  if (!marker.close()) return false;
  if (version == REKEY_VERSION_V2) {
    const uint32_t checksum = std::accumulate(
        paths.begin(), paths.end(), ClippingCodec::crc32(header.data(), header.size()),
        [](const uint32_t value, const std::string* path) {
          return ClippingCodec::crc32(reinterpret_cast<const uint8_t*>(path->data()), path->size(), value);
        });
    if (checksum != storedChecksum) return false;
  }
  if (!validRekeyIdentity(parsed, bookType)) return false;
  identity = std::move(parsed);
  return true;
}

bool writeRekeyIdentity(const std::string& markerPath, const std::string_view bookType, const RekeyIdentity& identity) {
  const std::vector<uint8_t> encoded = encodeRekeyIdentity(identity, bookType);
  if (encoded.empty()) return false;
  HalFile marker;
  if (!Storage.openFileForWrite("CLIP", markerPath, marker)) return false;
  const bool written = marker.write(encoded.data(), encoded.size()) == encoded.size();
  marker.flush();
  if (!written || !marker.sync() || !marker.close()) return false;
  RekeyIdentity verified;
  return readRekeyIdentity(markerPath, bookType, verified) && verified == identity;
}

struct HalSourceContext {
  HalFile* file = nullptr;
};

bool halReadAt(void* rawContext, const uint32_t offset, uint8_t* data, const size_t length) {
  auto& context = *static_cast<HalSourceContext*>(rawContext);
  return context.file && context.file->seek(offset) && context.file->read(data, length) == static_cast<int>(length);
}

ClippingCodec::Status inspectOpenFile(HalFile& file, ClippingCodec::Index& out) {
  const uint64_t fileLength = file.fileSize64();
  if (fileLength > ClippingCodec::MAX_FILE_BYTES || fileLength > std::numeric_limits<uint32_t>::max()) {
    return ClippingCodec::Status::LimitExceeded;
  }
  HalSourceContext context{&file};
  return ClippingCodec::inspect({&context, static_cast<uint32_t>(fileLength), halReadAt}, out);
}

ClippingCodec::Status inspectPath(const std::string& path, ClippingCodec::Index& out) {
  HalFile file;
  if (!Storage.openFileForRead("CLIP", path, file)) return ClippingCodec::Status::IoError;
  return inspectOpenFile(file, out);
}

bool unsupportedForWrite(const ClippingCodec::Status status) {
  return status == ClippingCodec::Status::NewerVersion || status == ClippingCodec::Status::UnsupportedVersion;
}

bool protectedSiblingForWrite(const ClippingCodec::Status status) {
  return unsupportedForWrite(status) || status == ClippingCodec::Status::IoError ||
         status == ClippingCodec::Status::LimitExceeded;
}

bool recoverCandidate(const std::string& candidatePath, const std::string& finalPath,
                      ClippingCodec::Index& recoveredIndex) {
  if (Storage.exists(finalPath.c_str()) && !Storage.remove(finalPath.c_str())) return false;
  if (!Storage.rename(candidatePath.c_str(), finalPath.c_str())) return false;
  return inspectPath(finalPath, recoveredIndex) == ClippingCodec::Status::Ok;
}

struct LoadCandidate {
  bool exists = false;
  ClippingCodec::Status status = ClippingCodec::Status::IoError;
  ClippingCodec::Index index;
};

LoadCandidate inspectCandidate(const std::string& path) {
  LoadCandidate candidate;
  candidate.exists = Storage.exists(path.c_str());
  if (candidate.exists) candidate.status = inspectPath(path, candidate.index);
  return candidate;
}

bool writeExact(HalFile& file, const uint8_t* data, const size_t length) {
  return length == 0 || file.write(data, length) == length;
}

bool sameBook(const ClippingCodec::BookMetadata& lhs, const ClippingCodec::BookMetadata& rhs) {
  return lhs.title == rhs.title && lhs.author == rhs.author && lhs.path == rhs.path && lhs.bookType == rhs.bookType;
}

bool sameClipping(const ClippingCodec::ClippingMetadata& lhs, const ClippingCodec::ClippingMetadata& rhs) {
  return lhs.spineIndex == rhs.spineIndex && lhs.startPage == rhs.startPage && lhs.endPage == rhs.endPage &&
         lhs.pageCount == rhs.pageCount && lhs.startWordIndex == rhs.startWordIndex &&
         lhs.endWordIndex == rhs.endWordIndex && lhs.wordCount == rhs.wordCount &&
         lhs.paragraphIndex == rhs.paragraphIndex && lhs.timestamp == rhs.timestamp &&
         lhs.textOffset == rhs.textOffset && lhs.textLength == rhs.textLength && lhs.chapterTitle == rhs.chapterTitle &&
         lhs.pageFingerprint == rhs.pageFingerprint;
}

bool sameClippings(const std::vector<ClippingCodec::ClippingMetadata>& lhs,
                   const std::vector<ClippingCodec::ClippingMetadata>& rhs) {
  return lhs.size() == rhs.size() &&
         std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                    [](const auto& left, const auto& right) { return sameClipping(left, right); });
}

const char* legacyBackupSuffix(const ClippingCodec::Format format) {
  switch (format) {
    case ClippingCodec::Format::CrossInkV1:
      return ".crossink-v1.orig";
    case ClippingCodec::Format::CrossInkV2:
      return ".crossink-v2.orig";
    case ClippingCodec::Format::Current:
      return nullptr;
  }
  return nullptr;
}

bool filesEqual(const std::string& leftPath, const std::string& rightPath) {
  HalFile left;
  HalFile right;
  if (!Storage.openFileForRead("CLIP", leftPath, left) || !Storage.openFileForRead("CLIP", rightPath, right) ||
      left.fileSize64() != right.fileSize64()) {
    return false;
  }

  std::array<uint8_t, 128> leftBuffer{};
  std::array<uint8_t, 128> rightBuffer{};
  uint64_t remaining = left.fileSize64();
  while (remaining > 0) {
    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(leftBuffer.size(), remaining));
    if (left.read(leftBuffer.data(), chunk) != static_cast<int>(chunk) ||
        right.read(rightBuffer.data(), chunk) != static_cast<int>(chunk) ||
        !std::equal(leftBuffer.begin(), leftBuffer.begin() + static_cast<std::ptrdiff_t>(chunk), rightBuffer.begin())) {
      return false;
    }
    remaining -= chunk;
  }
  return true;
}

bool isExactLegacyBackup(const std::string& canonicalPath, const std::string& backupPath,
                         const ClippingCodec::Format expectedFormat) {
  ClippingCodec::Index backup;
  return inspectPath(backupPath, backup) == ClippingCodec::Status::Ok && backup.format == expectedFormat &&
         filesEqual(canonicalPath, backupPath);
}

bool createExactLegacyBackup(const std::string& canonicalPath, const ClippingCodec::Format format) {
  const char* suffix = legacyBackupSuffix(format);
  if (!suffix) return false;
  const std::string backupPath = canonicalPath + suffix;
  const std::string tempPath = backupPath + ".tmp";

  // The versioned original is immutable. A pre-existing file must match the
  // canonical legacy bytes exactly; conflicts are preserved and fail closed.
  if (Storage.exists(backupPath.c_str())) return isExactLegacyBackup(canonicalPath, backupPath, format);

  if (Storage.exists(tempPath.c_str())) {
    if (!isExactLegacyBackup(canonicalPath, tempPath, format)) return false;
  } else {
    HalFile source;
    HalFile temp;
    if (!Storage.openFileForRead("CLIP", canonicalPath, source)) return false;
    if (!Storage.openFileForWrite("CLIP", tempPath, temp)) {
      source.close();
      return false;
    }

    std::array<uint8_t, 128> buffer{};
    uint64_t remaining = source.fileSize64();
    bool copied = true;
    while (remaining > 0) {
      const size_t chunk = static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining));
      if (source.read(buffer.data(), chunk) != static_cast<int>(chunk) || temp.write(buffer.data(), chunk) != chunk) {
        copied = false;
        break;
      }
      remaining -= chunk;
    }
    temp.flush();
    if (copied) copied = temp.sync();
    if (!temp.close()) copied = false;
    source.close();
    if (!copied || !isExactLegacyBackup(canonicalPath, tempPath, format)) {
      Storage.remove(tempPath.c_str());
      return false;
    }
  }

  if (!Storage.rename(tempPath.c_str(), backupPath.c_str())) return false;
  return isExactLegacyBackup(canonicalPath, backupPath, format);
}

bool sameRekeyedIndex(const ClippingCodec::Index& actual, const ClippingCodec::BookMetadata& expectedBook,
                      const std::vector<ClippingCodec::ClippingMetadata>& expectedClippings,
                      const uint32_t expectedFileLength) {
  return actual.format == ClippingCodec::Format::Current && actual.fileLength == expectedFileLength &&
         sameBook(actual.book, expectedBook) && sameClippings(actual.clippings, expectedClippings);
}

bool sameTextPayloads(HalFile& source, const ClippingCodec::Index& sourceIndex, HalFile& destination,
                      const ClippingCodec::Index& destinationIndex) {
  if (sourceIndex.clippings.size() != destinationIndex.clippings.size()) return false;
  std::array<uint8_t, 128> sourceBuffer{};
  std::array<uint8_t, 128> destinationBuffer{};
  for (size_t i = 0; i < sourceIndex.clippings.size(); ++i) {
    const auto& sourceClipping = sourceIndex.clippings[i];
    const auto& destinationClipping = destinationIndex.clippings[i];
    if (sourceClipping.textLength != destinationClipping.textLength) return false;

    uint32_t sourceOffset = sourceClipping.textOffset;
    uint32_t destinationOffset = destinationClipping.textOffset;
    uint16_t remaining = sourceClipping.textLength;
    while (remaining > 0) {
      const size_t chunk = std::min<size_t>(sourceBuffer.size(), remaining);
      if (!source.seek(sourceOffset) || source.read(sourceBuffer.data(), chunk) != static_cast<int>(chunk) ||
          !destination.seek(destinationOffset) ||
          destination.read(destinationBuffer.data(), chunk) != static_cast<int>(chunk) ||
          !std::equal(sourceBuffer.begin(), sourceBuffer.begin() + static_cast<std::ptrdiff_t>(chunk),
                      destinationBuffer.begin())) {
        return false;
      }
      sourceOffset += static_cast<uint32_t>(chunk);
      destinationOffset += static_cast<uint32_t>(chunk);
      remaining = static_cast<uint16_t>(remaining - chunk);
    }
  }
  return true;
}

}  // namespace

ClippingStore::LoadResult ClippingStore::loadForBook(const std::string& filePath, const std::string& title,
                                                     const std::string& author, const std::string& bookType) {
  unload();
  book_ = {title, author, filePath, bookType};
  storePath_ = ClippingCodec::filePathForBook(filePath, bookType);
  std::vector<uint8_t> metadataCheck;
  lastCodecStatus_ = ClippingCodec::encodeBookMetadata(book_, metadataCheck);
  if (storePath_.empty() || lastCodecStatus_ != ClippingCodec::Status::Ok) {
    unload();
    return LoadResult::InvalidFile;
  }
  const auto finishRecoveredRekey = [&]() {
    const std::string markerPath = storePath_ + ".move";
    RekeyIdentity identity;
    if (!Storage.exists(markerPath.c_str()) || !readRekeyIdentity(markerPath, bookType, identity) ||
        identity.destinationBook != filePath || identity.destinationStore != storePath_ ||
        Storage.exists(identity.sourceBook.c_str()) || !Storage.exists(identity.destinationBook.c_str())) {
      return;
    }
    if (removeFilesForBook(identity.sourceBook, bookType) && !Storage.remove(markerPath.c_str())) {
      LOG_ERR("CLIP", "Recovered re-key but could not remove its marker: %s", markerPath.c_str());
    }
  };

  const std::string backupPath = storePath_ + ".bak";
  const std::string tempPath = storePath_ + ".tmp";
  LoadCandidate canonical = inspectCandidate(storePath_);
  // A canonical file that cannot be inspected may be transiently unreadable
  // or belong to a format with a larger limit. Never delete it merely because
  // an older backup happens to be readable.
  if (canonical.exists && protectedSiblingForWrite(canonical.status)) {
    lastCodecStatus_ = canonical.status;
    if (unsupportedForWrite(canonical.status)) return LoadResult::UnsupportedVersion;
    return canonical.status == ClippingCodec::Status::IoError ? LoadResult::IoError : LoadResult::InvalidFile;
  }

  bool recovered = false;
  if (!canonical.exists || canonical.status != ClippingCodec::Status::Ok) {
    const LoadCandidate backup = inspectCandidate(backupPath);
    const LoadCandidate temp = inspectCandidate(tempPath);
    if ((backup.exists && protectedSiblingForWrite(backup.status)) ||
        (temp.exists && protectedSiblingForWrite(temp.status))) {
      lastCodecStatus_ = backup.exists && protectedSiblingForWrite(backup.status) ? backup.status : temp.status;
      if (unsupportedForWrite(lastCodecStatus_)) return LoadResult::UnsupportedVersion;
      return lastCodecStatus_ == ClippingCodec::Status::IoError ? LoadResult::IoError : LoadResult::InvalidFile;
    }

    const LoadCandidate* recovery = nullptr;
    std::string recoveryPath;
    if (backup.exists && backup.status == ClippingCodec::Status::Ok) {
      recovery = &backup;
      recoveryPath = backupPath;
    } else if (temp.exists && temp.status == ClippingCodec::Status::Ok) {
      recovery = &temp;
      recoveryPath = tempPath;
    }

    if (!recovery) {
      if (!canonical.exists && !backup.exists && !temp.exists) {
        loaded_ = true;
        index_.format = ClippingCodec::Format::Current;
        index_.book = book_;
        lastCodecStatus_ = ClippingCodec::Status::Ok;
        finishRecoveredRekey();
        return LoadResult::Ready;
      }
      lastCodecStatus_ = canonical.exists ? canonical.status : (backup.exists ? backup.status : temp.status);
      return lastCodecStatus_ == ClippingCodec::Status::IoError ? LoadResult::IoError : LoadResult::InvalidFile;
    }

    ClippingCodec::Index recoveredIndex;
    if (!recoverCandidate(recoveryPath, storePath_, recoveredIndex)) {
      lastCodecStatus_ = ClippingCodec::Status::IoError;
      return LoadResult::IoError;
    }
    canonical.exists = true;
    canonical.status = ClippingCodec::Status::Ok;
    canonical.index = std::move(recoveredIndex);
    recovered = true;
  }

  if (canonical.index.book.path != filePath || canonical.index.book.bookType != bookType) {
    lastCodecStatus_ = ClippingCodec::Status::Corrupt;
    return LoadResult::InvalidFile;
  }

  index_ = std::move(canonical.index);
  // The on-disk metadata is authoritative after full validation. This also
  // lets path-move code re-key an existing store without reparsing the EPUB
  // merely to rediscover its title and author.
  book_ = index_.book;
  loaded_ = true;
  lastCodecStatus_ = ClippingCodec::Status::Ok;
  finishRecoveredRekey();
  if (index_.format != ClippingCodec::Format::Current) {
    if (rewrite(index_.clippings, SIZE_MAX, nullptr)) return LoadResult::Migrated;
    // The fully validated legacy file remains canonical if migration could not
    // complete, so callers may still read its text without losing data. A
    // failed publish can call unload() when rollback cannot re-establish a
    // canonical file; cppcheck cannot see that mutation through rewrite().
    // cppcheck-suppress knownConditionTrueFalse
    if (!loaded_) return LoadResult::IoError;
    return LoadResult::LoadedLegacy;
  }
  return recovered ? LoadResult::Recovered : LoadResult::Loaded;
}

bool ClippingStore::removeFilesForBook(const std::string& filePath, const std::string& bookType) {
  const std::string canonical = ClippingCodec::filePathForBook(filePath, bookType);
  if (canonical.empty()) return false;
  const std::array<std::string, 3> paths = {canonical, canonical + ".bak", canonical + ".tmp"};
  const bool pathsOwned = std::all_of(paths.begin(), paths.end(), [&](const std::string& candidatePath) {
    if (!Storage.exists(candidatePath.c_str())) return true;
    const LoadCandidate candidate = inspectCandidate(candidatePath);
    return candidate.status == ClippingCodec::Status::Ok && candidate.index.book.path == filePath &&
           candidate.index.book.bookType == bookType;
  });
  if (!pathsOwned) {
    return false;
  }

  const std::string markerPath = canonical + ".move";
  const bool markerExists = Storage.exists(markerPath.c_str());
  if (markerExists) {
    RekeyIdentity identity;
    if (!readRekeyIdentity(markerPath, bookType, identity) || identity.destinationBook != filePath ||
        identity.destinationStore != canonical) {
      return false;
    }
  }

  bool removed = true;
  for (const std::string& candidatePath : paths) {
    if (Storage.exists(candidatePath.c_str())) removed = Storage.remove(candidatePath.c_str()) && removed;
  }
  // A marker is deleted only when its durable identity proves that this exact
  // destination owns it. Malformed or mismatched markers make the whole
  // operation fail closed before any validated sibling is removed.
  if (markerExists) {
    removed = Storage.remove(markerPath.c_str()) && removed;
  }
  return removed;
}

bool ClippingStore::hasFilesForBook(const std::string& filePath, const std::string& bookType) {
  const std::string canonical = ClippingCodec::filePathForBook(filePath, bookType);
  return !canonical.empty() &&
         (Storage.exists(canonical.c_str()) || Storage.exists((canonical + ".bak").c_str()) ||
          Storage.exists((canonical + ".tmp").c_str()) || Storage.exists((canonical + ".move").c_str()));
}

bool ClippingStore::quarantineFilesForBook(const std::string& filePath, const std::string& quarantineDirectory,
                                           const std::string& bookType) {
  const std::string canonical = ClippingCodec::filePathForBook(filePath, bookType);
  if (canonical.empty() || quarantineDirectory.empty() || quarantineDirectory == "/" ||
      !Storage.exists(quarantineDirectory.c_str())) {
    return false;
  }

  struct CandidatePath {
    std::string source;
    std::string orphanBase;
    bool marker = false;
  };
  const std::array<CandidatePath, 4> candidates = {
      CandidatePath{canonical, quarantineDirectory + "/.crossvi_replaced_clippings.bin", false},
      CandidatePath{canonical + ".bak", quarantineDirectory + "/.crossvi_replaced_clippings.bin.bak", false},
      CandidatePath{canonical + ".tmp", quarantineDirectory + "/.crossvi_replaced_clippings.bin.tmp", false},
      CandidatePath{canonical + ".move", quarantineDirectory + "/.crossvi_replaced_clippings.move", true},
  };

  const auto owned = [&](const CandidatePath& candidate, const std::string& path) {
    if (candidate.marker) {
      RekeyIdentity identity;
      return readRekeyIdentity(path, bookType, identity) && identity.destinationBook == filePath &&
             identity.destinationStore == canonical;
    }
    const LoadCandidate inspected = inspectCandidate(path);
    return inspected.status == ClippingCodec::Status::Ok && inspected.index.book.path == filePath &&
           inspected.index.book.bookType == bookType;
  };

  // Validate every active and previously quarantined candidate before moving
  // anything. A bad sibling must not turn a partial transaction into data loss.
  for (const CandidatePath& candidate : candidates) {
    const bool sourceExists = Storage.exists(candidate.source.c_str());
    const bool destinationExists = Storage.exists(candidate.orphanBase.c_str());
    if ((sourceExists && !owned(candidate, candidate.source)) ||
        (destinationExists && !owned(candidate, candidate.orphanBase)) || (sourceExists && destinationExists)) {
      return false;
    }
  }

  for (const CandidatePath& candidate : candidates) {
    if (!Storage.exists(candidate.source.c_str())) continue;
    if (!Storage.rename(candidate.source.c_str(), candidate.orphanBase.c_str()) ||
        !owned(candidate, candidate.orphanBase)) {
      return false;
    }
  }
  return true;
}

void ClippingStore::unload() {
  book_ = {};
  index_ = {};
  storePath_.clear();
  preparedDestinationPath_.clear();
  preparedIndex_ = {};
  loaded_ = false;
  lastCodecStatus_ = ClippingCodec::Status::Ok;
}

ClippingStore::AddResult ClippingStore::add(const ClippingCodec::ClippingMetadata& clipping,
                                            const std::string_view text) {
  if (!loaded_) return AddResult::SaveFailed;
  if (index_.clippings.size() >= ClippingCodec::MAX_CLIPPINGS_PER_BOOK) return AddResult::LimitReached;
  if (text.empty() || text.size() > ClippingCodec::MAX_TEXT_BYTES || !ClippingCodec::isValidUtf8(text)) {
    return AddResult::InvalidData;
  }

  std::vector<ClippingCodec::ClippingMetadata> target = index_.clippings;
  ClippingCodec::ClippingMetadata added = clipping;
  added.textOffset = 0;
  added.textLength = static_cast<uint16_t>(text.size());
  std::array<uint8_t, ClippingCodec::RECORD_SIZE> validation{};
  if (ClippingCodec::encodeRecord(added, validation) != ClippingCodec::Status::Ok) {
    return AddResult::InvalidData;
  }
  target.push_back(std::move(added));
  const size_t addedIndex = target.size() - 1;
  return rewrite(std::move(target), addedIndex, &text) ? AddResult::Added : AddResult::SaveFailed;
}

bool ClippingStore::remove(const size_t index) {
  if (!loaded_ || index >= index_.clippings.size()) return false;
  std::vector<ClippingCodec::ClippingMetadata> target = index_.clippings;
  target.erase(target.begin() + static_cast<std::ptrdiff_t>(index));
  return rewrite(std::move(target), SIZE_MAX, nullptr);
}

const ClippingCodec::ClippingMetadata* ClippingStore::at(const size_t index) const {
  return index < index_.clippings.size() ? &index_.clippings[index] : nullptr;
}

bool ClippingStore::readText(const size_t index, std::string& out) const {
  out.clear();
  const auto* clipping = at(index);
  if (!loaded_ || !clipping || storePath_.empty()) return false;

  HalFile file;
  if (!Storage.openFileForRead("CLIP", storePath_, file) || file.fileSize64() != index_.fileLength) return false;
  // The index is intentionally metadata-only, so the backing file may have
  // changed between loadForBook() and this lazy read. Reinspect the same open
  // handle to revalidate the stored payload CRC, then require the metadata to
  // still describe exactly the file that was loaded.
  ClippingCodec::Index verified;
  if (inspectOpenFile(file, verified) != ClippingCodec::Status::Ok || verified.format != index_.format ||
      verified.fileLength != index_.fileLength || !sameBook(verified.book, index_.book) ||
      !sameClippings(verified.clippings, index_.clippings)) {
    return false;
  }
  HalSourceContext context{&file};
  return ClippingCodec::readText({&context, verified.fileLength, halReadAt}, verified.clippings[index], out) ==
         ClippingCodec::Status::Ok;
}

ClippingStore::RekeyResult ClippingStore::prepareRekeyForBook(const std::string& filePath, const std::string& title,
                                                              const std::string& author, const std::string& bookType) {
  if (!loaded_) return RekeyResult::NotLoaded;
  if (index_.format != ClippingCodec::Format::Current && !createExactLegacyBackup(storePath_, index_.format)) {
    lastCodecStatus_ = ClippingCodec::Status::IoError;
    return RekeyResult::IoError;
  }

  ClippingCodec::BookMetadata destinationBook{title, author, filePath, bookType};
  std::vector<uint8_t> encodedMetadata;
  lastCodecStatus_ = ClippingCodec::encodeBookMetadata(destinationBook, encodedMetadata);
  std::string destinationPath = ClippingCodec::filePathForBook(filePath, bookType);
  if (lastCodecStatus_ != ClippingCodec::Status::Ok || destinationPath.empty()) {
    return RekeyResult::InvalidDestination;
  }
  if (bookType != book_.bookType) return RekeyResult::InvalidDestination;
  if (filePath == book_.path) return RekeyResult::Unchanged;
  // A CRC collision must not turn a move into an in-place metadata rewrite.
  if (destinationPath == storePath_) return RekeyResult::DestinationExists;
  if (!preparedDestinationPath_.empty()) {
    return destinationPath == preparedDestinationPath_ && preparedIndex_.book.path == filePath &&
                   preparedIndex_.book.bookType == bookType
               ? RekeyResult::Prepared
               : RekeyResult::DestinationExists;
  }

  const std::string destinationTemp = destinationPath + ".tmp";
  const std::string destinationBackup = destinationPath + ".bak";
  const std::string rekeyMarker = destinationPath + ".move";
  const RekeyIdentity rekeyIdentity{book_.path, filePath, storePath_, destinationPath};
  if (!validRekeyIdentity(rekeyIdentity, bookType)) return RekeyResult::InvalidDestination;
  bool markerReady = false;
  if (Storage.exists(rekeyMarker.c_str())) {
    RekeyIdentity storedIdentity;
    if (!readRekeyIdentity(rekeyMarker, bookType, storedIdentity)) {
      // A name-shaped sibling is not proof that CrossVi created it. Preserve
      // malformed or unreadable bytes rather than silently replacing a file
      // whose ownership cannot be established from its durable identity.
      return RekeyResult::DestinationExists;
    } else if (!(storedIdentity == rekeyIdentity)) {
      return RekeyResult::DestinationExists;
    } else {
      markerReady = true;
    }
    // Before the EPUB rename, this exact marker proves every destination
    // sibling is a non-authoritative copy from our interrupted transaction.
    // Rebuild it from the still-authoritative source so changed clippings and
    // partial writes can never block the move permanently.
    if (markerReady && Storage.exists(book_.path.c_str()) && !Storage.exists(filePath.c_str())) {
      const std::array<const std::string*, 3> preparedPaths = {&destinationPath, &destinationTemp, &destinationBackup};
      const bool cleanupFailed =
          std::any_of(preparedPaths.begin(), preparedPaths.end(), [](const std::string* preparedPath) {
            return Storage.exists(preparedPath->c_str()) && !Storage.remove(preparedPath->c_str());
          });
      if (cleanupFailed) return RekeyResult::IoError;
      if (!Storage.remove(rekeyMarker.c_str())) return RekeyResult::IoError;
      markerReady = false;
    }
  }
  const LoadCandidate existingDestination = inspectCandidate(destinationPath);
  const LoadCandidate existingTemp = inspectCandidate(destinationTemp);
  if (existingDestination.exists && unsupportedForWrite(existingDestination.status)) {
    lastCodecStatus_ = existingDestination.status;
    return RekeyResult::UnsupportedVersion;
  }
  if (existingTemp.exists && unsupportedForWrite(existingTemp.status)) {
    lastCodecStatus_ = existingTemp.status;
    return RekeyResult::UnsupportedVersion;
  }
  if (existingDestination.exists && existingTemp.exists) return RekeyResult::DestinationExists;
  if (Storage.exists(destinationBackup.c_str())) {
    const LoadCandidate backup = inspectCandidate(destinationBackup);
    lastCodecStatus_ = backup.status;
    return unsupportedForWrite(backup.status) ? RekeyResult::UnsupportedVersion : RekeyResult::DestinationExists;
  }

  const bool sourceExists = Storage.exists(storePath_.c_str());
  if (!sourceExists) {
    // A freshly loaded empty store has no canonical bytes to copy. Remember
    // only the prepared key; finalize switches the in-memory binding after the
    // EPUB rename succeeds.
    if (index_.fileLength != 0 || !index_.clippings.empty()) {
      lastCodecStatus_ = ClippingCodec::Status::IoError;
      return RekeyResult::SourceInvalid;
    }
    if (existingDestination.exists || existingTemp.exists) return RekeyResult::DestinationExists;
    if ((!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) ||
        (!Storage.exists(ClippingCodec::DIRECTORY) && !Storage.mkdir(ClippingCodec::DIRECTORY)) ||
        (!markerReady && !writeRekeyIdentity(rekeyMarker, bookType, rekeyIdentity))) {
      return RekeyResult::IoError;
    }
    preparedIndex_ = index_;
    preparedIndex_.book = std::move(destinationBook);
    preparedDestinationPath_ = std::move(destinationPath);
    lastCodecStatus_ = ClippingCodec::Status::Ok;
    return RekeyResult::Prepared;
  }

  HalFile sourceFile;
  ClippingCodec::Index sourceIndex;
  if (!Storage.openFileForRead("CLIP", storePath_, sourceFile)) {
    lastCodecStatus_ = ClippingCodec::Status::IoError;
    return RekeyResult::IoError;
  }
  lastCodecStatus_ = inspectOpenFile(sourceFile, sourceIndex);
  if (lastCodecStatus_ != ClippingCodec::Status::Ok || sourceIndex.fileLength != index_.fileLength ||
      sourceIndex.book.path != book_.path || sourceIndex.book.bookType != book_.bookType ||
      !sameClippings(sourceIndex.clippings, index_.clippings)) {
    return unsupportedForWrite(lastCodecStatus_) ? RekeyResult::UnsupportedVersion : RekeyResult::SourceInvalid;
  }

  std::vector<ClippingCodec::ClippingMetadata> target = sourceIndex.clippings;
  const uint64_t recordsOffset64 = ClippingCodec::HEADER_SIZE + encodedMetadata.size();
  const uint64_t textsOffset64 = recordsOffset64 + target.size() * ClippingCodec::RECORD_SIZE;
  uint64_t fileLength64 = textsOffset64;
  for (auto& clipping : target) {
    if (clipping.textLength == 0 || clipping.textLength > ClippingCodec::MAX_TEXT_BYTES ||
        fileLength64 > std::numeric_limits<uint32_t>::max() - clipping.textLength) {
      lastCodecStatus_ = ClippingCodec::Status::LimitExceeded;
      return RekeyResult::SourceInvalid;
    }
    clipping.textOffset = static_cast<uint32_t>(fileLength64);
    fileLength64 += clipping.textLength;
  }
  if (fileLength64 > ClippingCodec::MAX_FILE_BYTES || textsOffset64 > std::numeric_limits<uint32_t>::max()) {
    lastCodecStatus_ = ClippingCodec::Status::LimitExceeded;
    return RekeyResult::SourceInvalid;
  }

  std::array<uint8_t, ClippingCodec::RECORD_SIZE> encodedRecord{};
  for (const auto& clipping : target) {
    lastCodecStatus_ = ClippingCodec::encodeRecord(clipping, encodedRecord);
    if (lastCodecStatus_ != ClippingCodec::Status::Ok) return RekeyResult::SourceInvalid;
  }

  // A complete destination left by a reset during an earlier prepare is safe
  // to reuse only when its metadata and every text payload match this source.
  const LoadCandidate* existingPrepared =
      existingDestination.exists ? &existingDestination : (existingTemp.exists ? &existingTemp : nullptr);
  const std::string* existingPreparedPath = existingDestination.exists ? &destinationPath : &destinationTemp;
  if (existingPrepared) {
    HalFile destinationFile;
    const bool reusable =
        existingPrepared->status == ClippingCodec::Status::Ok &&
        sameRekeyedIndex(existingPrepared->index, destinationBook, target, static_cast<uint32_t>(fileLength64)) &&
        Storage.openFileForRead("CLIP", *existingPreparedPath, destinationFile) &&
        sameTextPayloads(sourceFile, sourceIndex, destinationFile, existingPrepared->index);
    if (destinationFile) destinationFile.close();
    sourceFile.close();
    if (!reusable) {
      lastCodecStatus_ = existingPrepared->status;
      return RekeyResult::DestinationExists;
    }
    ClippingCodec::Index verified = existingPrepared->index;
    if (existingTemp.exists) {
      if (!Storage.rename(destinationTemp.c_str(), destinationPath.c_str())) {
        lastCodecStatus_ = ClippingCodec::Status::IoError;
        return RekeyResult::IoError;
      }
      lastCodecStatus_ = inspectPath(destinationPath, verified);
      if (lastCodecStatus_ != ClippingCodec::Status::Ok ||
          !sameRekeyedIndex(verified, destinationBook, target, static_cast<uint32_t>(fileLength64))) {
        return RekeyResult::IoError;
      }
    }
    if ((!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) ||
        (!Storage.exists(ClippingCodec::DIRECTORY) && !Storage.mkdir(ClippingCodec::DIRECTORY)) ||
        (!markerReady && !writeRekeyIdentity(rekeyMarker, bookType, rekeyIdentity))) {
      return RekeyResult::IoError;
    }
    preparedDestinationPath_ = std::move(destinationPath);
    preparedIndex_ = std::move(verified);
    lastCodecStatus_ = ClippingCodec::Status::Ok;
    return RekeyResult::Prepared;
  }

  if ((!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) ||
      (!Storage.exists(ClippingCodec::DIRECTORY) && !Storage.mkdir(ClippingCodec::DIRECTORY)) ||
      (!markerReady && !writeRekeyIdentity(rekeyMarker, bookType, rekeyIdentity))) {
    lastCodecStatus_ = ClippingCodec::Status::IoError;
    return RekeyResult::IoError;
  }

  HalFile output;
  if (!Storage.openFileForWrite("CLIP", destinationTemp, output)) {
    lastCodecStatus_ = ClippingCodec::Status::IoError;
    return RekeyResult::IoError;
  }

  const uint32_t fileLength = static_cast<uint32_t>(fileLength64);
  const uint32_t recordsOffset = static_cast<uint32_t>(recordsOffset64);
  const uint32_t textsOffset = static_cast<uint32_t>(textsOffset64);
  std::array<uint8_t, ClippingCodec::HEADER_SIZE> header{};
  ClippingCodec::encodeHeader(static_cast<uint16_t>(target.size()), fileLength, 0,
                              static_cast<uint32_t>(encodedMetadata.size()), recordsOffset, textsOffset, header);
  bool writeOk = writeExact(output, header.data(), header.size());
  uint32_t payloadChecksum = 0;
  const auto writePayload = [&](const uint8_t* data, const size_t length) {
    if (!writeExact(output, data, length)) return false;
    payloadChecksum = ClippingCodec::crc32(data, length, payloadChecksum);
    return true;
  };

  if (writeOk) writeOk = writePayload(encodedMetadata.data(), encodedMetadata.size());
  for (size_t i = 0; writeOk && i < target.size(); ++i) {
    writeOk = ClippingCodec::encodeRecord(target[i], encodedRecord) == ClippingCodec::Status::Ok &&
              writePayload(encodedRecord.data(), encodedRecord.size());
  }

  std::array<uint8_t, 128> copyBuffer{};
  for (size_t i = 0; writeOk && i < target.size(); ++i) {
    uint32_t sourceOffset = sourceIndex.clippings[i].textOffset;
    uint16_t remaining = sourceIndex.clippings[i].textLength;
    while (writeOk && remaining > 0) {
      const size_t chunk = std::min<size_t>(copyBuffer.size(), remaining);
      writeOk = sourceFile.seek(sourceOffset) && sourceFile.read(copyBuffer.data(), chunk) == static_cast<int>(chunk) &&
                writePayload(copyBuffer.data(), chunk);
      sourceOffset += static_cast<uint32_t>(chunk);
      remaining = static_cast<uint16_t>(remaining - chunk);
    }
  }

  if (writeOk && output.position() == fileLength) {
    ClippingCodec::encodeHeader(static_cast<uint16_t>(target.size()), fileLength, payloadChecksum,
                                static_cast<uint32_t>(encodedMetadata.size()), recordsOffset, textsOffset, header);
    writeOk = output.seek(0) && writeExact(output, header.data(), header.size()) && output.sync() &&
              output.fileSize64() == fileLength;
  } else {
    writeOk = false;
  }
  output.close();
  if (!writeOk) {
    Storage.remove(destinationTemp.c_str());
    lastCodecStatus_ = ClippingCodec::Status::IoError;
    return RekeyResult::IoError;
  }

  ClippingCodec::Index verifiedTemp;
  lastCodecStatus_ = inspectPath(destinationTemp, verifiedTemp);
  if (lastCodecStatus_ != ClippingCodec::Status::Ok ||
      !sameRekeyedIndex(verifiedTemp, destinationBook, target, fileLength)) {
    Storage.remove(destinationTemp.c_str());
    if (lastCodecStatus_ == ClippingCodec::Status::Ok) lastCodecStatus_ = ClippingCodec::Status::Corrupt;
    return RekeyResult::IoError;
  }

  if (!Storage.rename(destinationTemp.c_str(), destinationPath.c_str())) {
    Storage.remove(destinationTemp.c_str());
    lastCodecStatus_ = ClippingCodec::Status::IoError;
    return RekeyResult::IoError;
  }

  ClippingCodec::Index verifiedFinal;
  lastCodecStatus_ = inspectPath(destinationPath, verifiedFinal);
  bool finalOk = lastCodecStatus_ == ClippingCodec::Status::Ok &&
                 sameRekeyedIndex(verifiedFinal, destinationBook, target, fileLength);
  HalFile destinationFile;
  if (finalOk) {
    finalOk = Storage.openFileForRead("CLIP", destinationPath, destinationFile) &&
              sameTextPayloads(sourceFile, sourceIndex, destinationFile, verifiedFinal);
  }
  if (destinationFile) destinationFile.close();
  sourceFile.close();
  if (!finalOk) {
    Storage.remove(destinationPath.c_str());
    if (lastCodecStatus_ == ClippingCodec::Status::Ok) lastCodecStatus_ = ClippingCodec::Status::Corrupt;
    return RekeyResult::IoError;
  }

  // Phase 1 deliberately leaves the old canonical untouched. A reset before
  // the EPUB rename therefore still opens the old key; a reset after it opens
  // this already-verified destination key.
  preparedDestinationPath_ = std::move(destinationPath);
  preparedIndex_ = std::move(verifiedFinal);
  lastCodecStatus_ = ClippingCodec::Status::Ok;
  return RekeyResult::Prepared;
}

ClippingStore::RekeyResult ClippingStore::finalizePreparedRekey() {
  if (!loaded_) return RekeyResult::NotLoaded;
  if (preparedDestinationPath_.empty()) return RekeyResult::NotPrepared;

  if (preparedIndex_.fileLength == 0) {
    if (Storage.exists(preparedDestinationPath_.c_str())) {
      lastCodecStatus_ = ClippingCodec::Status::IoError;
      return RekeyResult::IoError;
    }
  } else {
    ClippingCodec::Index verified;
    lastCodecStatus_ = inspectPath(preparedDestinationPath_, verified);
    if (lastCodecStatus_ != ClippingCodec::Status::Ok ||
        !sameRekeyedIndex(verified, preparedIndex_.book, preparedIndex_.clippings, preparedIndex_.fileLength)) {
      return unsupportedForWrite(lastCodecStatus_) ? RekeyResult::UnsupportedVersion : RekeyResult::IoError;
    }
    preparedIndex_ = std::move(verified);
  }

  const std::string sourcePath = storePath_;
  const std::string markerPath = preparedDestinationPath_ + ".move";
  const RekeyIdentity expectedIdentity{book_.path, preparedIndex_.book.path, sourcePath, preparedDestinationPath_};
  RekeyIdentity storedIdentity;
  if (!validRekeyIdentity(expectedIdentity, book_.bookType) ||
      !readRekeyIdentity(markerPath, book_.bookType, storedIdentity) || !(storedIdentity == expectedIdentity)) {
    lastCodecStatus_ = ClippingCodec::Status::IoError;
    return RekeyResult::IoError;
  }
  storePath_ = std::move(preparedDestinationPath_);
  book_ = preparedIndex_.book;
  index_ = std::move(preparedIndex_);
  preparedDestinationPath_.clear();
  preparedIndex_ = {};

  // The destination is authoritative once the EPUB rename has succeeded.
  // Failure to remove the duplicate source is non-fatal and preserves data.
  if (Storage.exists(sourcePath.c_str())) {
    const LoadCandidate source = inspectCandidate(sourcePath);
    if (source.status != ClippingCodec::Status::Ok || source.index.book.path != expectedIdentity.sourceBook ||
        source.index.book.bookType != book_.bookType) {
      LOG_ERR("CLIP", "Preserving invalid old clipping source after finalized re-key: %s", sourcePath.c_str());
    } else if (!Storage.remove(sourcePath.c_str())) {
      LOG_ERR("CLIP", "Could not remove old clipping source after finalized re-key: %s", sourcePath.c_str());
    }
  }
  if (Storage.exists(markerPath.c_str()) && !Storage.remove(markerPath.c_str())) {
    LOG_ERR("CLIP", "Finalized clipping re-key but could not remove marker: %s", markerPath.c_str());
  }
  lastCodecStatus_ = ClippingCodec::Status::Ok;
  return RekeyResult::Rekeyed;
}

bool ClippingStore::cancelPreparedRekey() {
  if (preparedDestinationPath_.empty()) return true;
  const RekeyIdentity expectedIdentity{book_.path, preparedIndex_.book.path, storePath_, preparedDestinationPath_};
  RekeyIdentity storedIdentity;
  const std::string markerPath = preparedDestinationPath_ + ".move";
  if (!validRekeyIdentity(expectedIdentity, book_.bookType) ||
      !readRekeyIdentity(markerPath, book_.bookType, storedIdentity) || !(storedIdentity == expectedIdentity) ||
      !removeFilesForBook(expectedIdentity.destinationBook, book_.bookType)) {
    lastCodecStatus_ = ClippingCodec::Status::IoError;
    return false;
  }
  preparedDestinationPath_.clear();
  preparedIndex_ = {};
  lastCodecStatus_ = ClippingCodec::Status::Ok;
  return true;
}

ClippingStore::RekeyResult ClippingStore::rekeyForBook(const std::string& filePath, const std::string& title,
                                                       const std::string& author, const std::string& bookType) {
  const RekeyResult prepared = prepareRekeyForBook(filePath, title, author, bookType);
  return prepared == RekeyResult::Prepared ? finalizePreparedRekey() : prepared;
}

bool ClippingStore::rewrite(std::vector<ClippingCodec::ClippingMetadata> target, const size_t replacementIndex,
                            const std::string_view* replacementText) {
  if (!loaded_ || !preparedDestinationPath_.empty() || target.size() > ClippingCodec::MAX_CLIPPINGS_PER_BOOK ||
      ((replacementText == nullptr) != (replacementIndex == SIZE_MAX)) ||
      (replacementText && replacementIndex >= target.size())) {
    return false;
  }
  if (index_.format != ClippingCodec::Format::Current && !createExactLegacyBackup(storePath_, index_.format)) {
    lastCodecStatus_ = ClippingCodec::Status::IoError;
    return false;
  }

  std::vector<uint8_t> encodedMetadata;
  lastCodecStatus_ = ClippingCodec::encodeBookMetadata(book_, encodedMetadata);
  if (lastCodecStatus_ != ClippingCodec::Status::Ok) return false;

  const bool canonicalExists = Storage.exists(storePath_.c_str());
  HalFile sourceFile;
  ClippingCodec::Index sourceIndex;
  if (canonicalExists) {
    if (!Storage.openFileForRead("CLIP", storePath_, sourceFile)) {
      lastCodecStatus_ = ClippingCodec::Status::IoError;
      return false;
    }
    lastCodecStatus_ = inspectOpenFile(sourceFile, sourceIndex);
    if (lastCodecStatus_ != ClippingCodec::Status::Ok || sourceIndex.fileLength != index_.fileLength ||
        sourceIndex.book.path != book_.path || sourceIndex.book.bookType != book_.bookType) {
      return false;
    }
  } else if (index_.fileLength != 0 || (!target.empty() && !replacementText)) {
    lastCodecStatus_ = ClippingCodec::Status::IoError;
    return false;
  }

  std::array<uint32_t, ClippingCodec::MAX_CLIPPINGS_PER_BOOK> sourceOffsets{};
  std::array<uint16_t, ClippingCodec::MAX_CLIPPINGS_PER_BOOK> sourceLengths{};
  const uint64_t recordsOffset64 = ClippingCodec::HEADER_SIZE + encodedMetadata.size();
  const uint64_t textsOffset64 = recordsOffset64 + target.size() * ClippingCodec::RECORD_SIZE;
  uint64_t fileLength64 = textsOffset64;
  for (size_t i = 0; i < target.size(); ++i) {
    sourceOffsets[i] = target[i].textOffset;
    sourceLengths[i] = target[i].textLength;
    const size_t textLength = replacementText && i == replacementIndex ? replacementText->size() : target[i].textLength;
    if (textLength == 0 || textLength > ClippingCodec::MAX_TEXT_BYTES ||
        fileLength64 > std::numeric_limits<uint32_t>::max() - textLength) {
      lastCodecStatus_ = ClippingCodec::Status::LimitExceeded;
      return false;
    }
    target[i].textOffset = static_cast<uint32_t>(fileLength64);
    target[i].textLength = static_cast<uint16_t>(textLength);
    fileLength64 += textLength;
  }
  if (fileLength64 > ClippingCodec::MAX_FILE_BYTES || textsOffset64 > std::numeric_limits<uint32_t>::max()) {
    lastCodecStatus_ = ClippingCodec::Status::LimitExceeded;
    return false;
  }

  std::array<uint8_t, ClippingCodec::RECORD_SIZE> encodedRecord{};
  for (const auto& clipping : target) {
    lastCodecStatus_ = ClippingCodec::encodeRecord(clipping, encodedRecord);
    if (lastCodecStatus_ != ClippingCodec::Status::Ok) return false;
  }

  if (!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) return false;
  if (!Storage.exists(ClippingCodec::DIRECTORY) && !Storage.mkdir(ClippingCodec::DIRECTORY)) return false;
  const std::string tempPath = storePath_ + ".tmp";
  const std::string backupPath = storePath_ + ".bak";
  for (const std::string* sibling : {&tempPath, &backupPath}) {
    if (!Storage.exists(sibling->c_str())) continue;
    ClippingCodec::Index siblingIndex;
    const ClippingCodec::Status siblingStatus = inspectPath(*sibling, siblingIndex);
    if (protectedSiblingForWrite(siblingStatus)) {
      lastCodecStatus_ = siblingStatus;
      return false;
    }
  }
  if (Storage.exists(tempPath.c_str()) && !Storage.remove(tempPath.c_str())) return false;

  HalFile output;
  if (!Storage.openFileForWrite("CLIP", tempPath, output)) {
    lastCodecStatus_ = ClippingCodec::Status::IoError;
    return false;
  }

  const uint32_t fileLength = static_cast<uint32_t>(fileLength64);
  const uint32_t recordsOffset = static_cast<uint32_t>(recordsOffset64);
  const uint32_t textsOffset = static_cast<uint32_t>(textsOffset64);
  std::array<uint8_t, ClippingCodec::HEADER_SIZE> header{};
  ClippingCodec::encodeHeader(static_cast<uint16_t>(target.size()), fileLength, 0,
                              static_cast<uint32_t>(encodedMetadata.size()), recordsOffset, textsOffset, header);
  bool writeOk = writeExact(output, header.data(), header.size());
  uint32_t payloadChecksum = 0;
  auto writePayload = [&](const uint8_t* data, const size_t length) {
    if (!writeExact(output, data, length)) return false;
    payloadChecksum = ClippingCodec::crc32(data, length, payloadChecksum);
    return true;
  };

  if (writeOk) writeOk = writePayload(encodedMetadata.data(), encodedMetadata.size());
  for (size_t i = 0; writeOk && i < target.size(); ++i) {
    writeOk = ClippingCodec::encodeRecord(target[i], encodedRecord) == ClippingCodec::Status::Ok &&
              writePayload(encodedRecord.data(), encodedRecord.size());
  }

  std::array<uint8_t, 128> copyBuffer{};
  for (size_t i = 0; writeOk && i < target.size(); ++i) {
    if (replacementText && i == replacementIndex) {
      writeOk = writePayload(reinterpret_cast<const uint8_t*>(replacementText->data()), replacementText->size());
      continue;
    }
    uint32_t sourceOffset = sourceOffsets[i];
    uint16_t remaining = sourceLengths[i];
    while (writeOk && remaining > 0) {
      const size_t chunk = std::min<size_t>(copyBuffer.size(), remaining);
      writeOk = sourceFile && sourceFile.seek(sourceOffset) &&
                sourceFile.read(copyBuffer.data(), chunk) == static_cast<int>(chunk) &&
                writePayload(copyBuffer.data(), chunk);
      sourceOffset += static_cast<uint32_t>(chunk);
      remaining = static_cast<uint16_t>(remaining - chunk);
    }
  }

  if (writeOk && output.position() == fileLength) {
    ClippingCodec::encodeHeader(static_cast<uint16_t>(target.size()), fileLength, payloadChecksum,
                                static_cast<uint32_t>(encodedMetadata.size()), recordsOffset, textsOffset, header);
    writeOk = output.seek(0) && writeExact(output, header.data(), header.size()) && output.sync() &&
              output.fileSize64() == fileLength;
  } else {
    writeOk = false;
  }
  output.close();
  if (sourceFile) sourceFile.close();
  if (!writeOk) {
    Storage.remove(tempPath.c_str());
    lastCodecStatus_ = ClippingCodec::Status::IoError;
    return false;
  }

  ClippingCodec::Index verifiedTemp;
  lastCodecStatus_ = inspectPath(tempPath, verifiedTemp);
  if (lastCodecStatus_ != ClippingCodec::Status::Ok || verifiedTemp.fileLength != fileLength ||
      verifiedTemp.book.path != book_.path || verifiedTemp.clippings.size() != target.size()) {
    Storage.remove(tempPath.c_str());
    return false;
  }

  if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) return false;
  if (canonicalExists && !Storage.rename(storePath_.c_str(), backupPath.c_str())) {
    Storage.remove(tempPath.c_str());
    return false;
  }
  if (!Storage.rename(tempPath.c_str(), storePath_.c_str())) {
    if (canonicalExists && !Storage.rename(backupPath.c_str(), storePath_.c_str())) {
      // Both validated files remain recoverable as .bak/.tmp, but the in-memory
      // index no longer has an authoritative canonical file. Refuse all access
      // until loadForBook() performs recovery.
      unload();
      lastCodecStatus_ = ClippingCodec::Status::IoError;
    }
    return false;
  }

  ClippingCodec::Index verifiedFinal;
  lastCodecStatus_ = inspectPath(storePath_, verifiedFinal);
  if (lastCodecStatus_ != ClippingCodec::Status::Ok || verifiedFinal.fileLength != fileLength ||
      verifiedFinal.book.path != book_.path || verifiedFinal.clippings.size() != target.size()) {
    const bool removedInvalidFinal = Storage.remove(storePath_.c_str());
    const bool restoredCanonical =
        !canonicalExists || (removedInvalidFinal && Storage.rename(backupPath.c_str(), storePath_.c_str()));
    if (!removedInvalidFinal || !restoredCanonical) {
      // Never keep serving the stale in-memory index when rollback could not
      // establish which on-disk file is canonical. The valid backup is left in
      // place for loadForBook() to recover.
      unload();
      lastCodecStatus_ = ClippingCodec::Status::IoError;
    }
    return false;
  }
  if (canonicalExists && Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
    LOG_ERR("CLIP", "Saved clippings but could not remove backup: %s", backupPath.c_str());
  }

  index_ = std::move(verifiedFinal);
  lastCodecStatus_ = ClippingCodec::Status::Ok;
  return true;
}
