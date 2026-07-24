#include "BookPathMoveUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "BookCacheUtils.h"
#include "BookReplacementTransaction.h"
#include "BookmarkUtil.h"
#include "CrossPointState.h"
#include "Epub/SourceIdentityStore.h"
#include "RecentBooksStore.h"
#include "activities/reader/ReadingStatsCompletionTransaction.h"
#include "clippings/ClippingStore.h"

namespace {
constexpr char CACHE_ROOT[] = "/.crosspoint";
constexpr char REPLACED_BOOK_SUFFIX[] = ".crossvi-replace.bak";
constexpr char REPLACEMENT_PENDING_SUFFIX[] = ".crossvi-replace.pending";
constexpr std::array<uint8_t, 4> REPLACEMENT_PENDING_MAGIC = {'C', 'V', 'R', 'P'};
constexpr uint8_t REPLACEMENT_PENDING_VERSION = 2;
constexpr size_t REPLACEMENT_PENDING_HEADER_SIZE =
    REPLACEMENT_PENDING_MAGIC.size() + 1 + 1 + sizeof(uint16_t) + sizeof(uint32_t);
constexpr size_t MAX_REPLACEMENT_PATH_BYTES = 512;

enum class CacheBackedBookKind : uint8_t { None, Epub, Xtc, Text };

bool hasCompletionTrackedStats(const CacheBackedBookKind kind) {
  return kind == CacheBackedBookKind::Epub || kind == CacheBackedBookKind::Text;
}

CacheBackedBookKind cacheBackedBookKind(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) return CacheBackedBookKind::Epub;
  if (FsHelpers::hasXtcExtension(path)) return CacheBackedBookKind::Xtc;
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) return CacheBackedBookKind::Text;
  return CacheBackedBookKind::None;
}

bool removeIfPresent(const std::string& path) { return !Storage.exists(path.c_str()) || Storage.remove(path.c_str()); }

void logCleanupFailure(const char* kind, const std::string& path) {
  LOG_ERR("BookMove", "Could not remove %s state for %s", kind, path.c_str());
}

std::string bookCachePath(const std::string& bookPath) {
  if (FsHelpers::hasEpubExtension(bookPath)) return Epub(bookPath, CACHE_ROOT).getCachePath();
  if (FsHelpers::hasXtcExtension(bookPath)) return Xtc(bookPath, CACHE_ROOT).getCachePath();
  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    return Txt(bookPath, CACHE_ROOT).getCachePath();
  }
  return {};
}

bool hasBookUserState(const std::string& bookPath, const std::string& cachePath) {
  const CacheBackedBookKind kind = cacheBackedBookKind(bookPath);
  return Storage.exists(cachePath.c_str()) || Storage.exists(BookmarkUtil::getBookmarkPath(bookPath).c_str()) ||
         Storage.exists(BookmarkUtil::getLegacyBookmarkPath(bookPath).c_str()) ||
         (kind == CacheBackedBookKind::Epub && ClippingStore::hasFilesForBook(bookPath)) ||
         (kind == CacheBackedBookKind::Text && ClippingStore::hasFilesForBook(bookPath, "txt"));
}

uint16_t readUint16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8U);
}

uint32_t readUint32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8U) |
         (static_cast<uint32_t>(bytes[2]) << 16U) | (static_cast<uint32_t>(bytes[3]) << 24U);
}

void writeUint16(uint8_t* bytes, const uint16_t value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8U);
}

void writeUint32(uint8_t* bytes, const uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8U);
  bytes[2] = static_cast<uint8_t>(value >> 16U);
  bytes[3] = static_cast<uint8_t>(value >> 24U);
}

uint32_t replacementPendingCrc(const uint8_t* bytes, const size_t length) {
  uint32_t crc = UINT32_MAX;
  for (size_t index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

bool validReplacementBookPath(const std::string& bookPath) {
  return !bookPath.empty() && bookPath != "/" && bookPath.size() <= MAX_REPLACEMENT_PATH_BYTES &&
         bookPath.find('\0') == std::string::npos;
}

bool readReplacementPending(const std::string& path, const std::string& expectedBookPath, bool& hadBook) {
  if (!validReplacementBookPath(expectedBookPath)) return false;
  HalFile file;
  if (!Storage.openFileForRead("BookMove", path, file)) return false;
  const uint64_t fileSize = file.fileSize64();
  if (fileSize < REPLACEMENT_PENDING_HEADER_SIZE ||
      fileSize > REPLACEMENT_PENDING_HEADER_SIZE + MAX_REPLACEMENT_PATH_BYTES) {
    file.close();
    return false;
  }

  std::array<uint8_t, REPLACEMENT_PENDING_HEADER_SIZE> header{};
  if (file.read(header.data(), header.size()) != static_cast<int>(header.size()) ||
      !std::equal(REPLACEMENT_PENDING_MAGIC.begin(), REPLACEMENT_PENDING_MAGIC.end(), header.begin()) ||
      header[REPLACEMENT_PENDING_MAGIC.size()] != REPLACEMENT_PENDING_VERSION ||
      (header[REPLACEMENT_PENDING_MAGIC.size() + 1] & ~1U) != 0) {
    if (file) file.close();
    return false;
  }
  const size_t pathLength = readUint16(header.data() + REPLACEMENT_PENDING_MAGIC.size() + 2);
  if (pathLength == 0 || pathLength > MAX_REPLACEMENT_PATH_BYTES ||
      fileSize != REPLACEMENT_PENDING_HEADER_SIZE + pathLength) {
    file.close();
    return false;
  }
  std::string storedBookPath(pathLength, '\0');
  if (file.read(storedBookPath.data(), storedBookPath.size()) != static_cast<int>(storedBookPath.size())) {
    file.close();
    return false;
  }
  const bool closed = file.close();
  const uint32_t storedCrc = readUint32(header.data() + REPLACEMENT_PENDING_MAGIC.size() + 2 + sizeof(uint16_t));
  const uint8_t flags = header[REPLACEMENT_PENDING_MAGIC.size() + 1];
  std::vector<uint8_t> payload(1 + storedBookPath.size());
  payload[0] = flags;
  std::copy(storedBookPath.begin(), storedBookPath.end(), payload.begin() + 1);
  if (!closed || storedBookPath != expectedBookPath || storedBookPath.find('\0') != std::string::npos ||
      replacementPendingCrc(payload.data(), payload.size()) != storedCrc) {
    return false;
  }
  hadBook = (flags & 1U) != 0;
  return true;
}

bool writeReplacementPending(const std::string& path, const std::string& bookPath, const bool hadBook) {
  if (!validReplacementBookPath(bookPath)) return false;
  if (Storage.exists(path.c_str())) {
    bool storedHadBook = false;
    return readReplacementPending(path, bookPath, storedHadBook) && storedHadBook == hadBook;
  }
  std::vector<uint8_t> bytes(REPLACEMENT_PENDING_HEADER_SIZE + bookPath.size());
  std::copy(REPLACEMENT_PENDING_MAGIC.begin(), REPLACEMENT_PENDING_MAGIC.end(), bytes.begin());
  bytes[REPLACEMENT_PENDING_MAGIC.size()] = REPLACEMENT_PENDING_VERSION;
  const uint8_t flags = hadBook ? 1U : 0U;
  bytes[REPLACEMENT_PENDING_MAGIC.size() + 1] = flags;
  writeUint16(bytes.data() + REPLACEMENT_PENDING_MAGIC.size() + 2, static_cast<uint16_t>(bookPath.size()));
  std::vector<uint8_t> payload(1 + bookPath.size());
  payload[0] = flags;
  std::copy(bookPath.begin(), bookPath.end(), payload.begin() + 1);
  writeUint32(bytes.data() + REPLACEMENT_PENDING_MAGIC.size() + 2 + sizeof(uint16_t),
              replacementPendingCrc(payload.data(), payload.size()));
  std::copy(bookPath.begin(), bookPath.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(REPLACEMENT_PENDING_HEADER_SIZE));
  HalFile file;
  if (!Storage.openFileForWrite("BookMove", path, file) || file.write(bytes.data(), bytes.size()) != bytes.size()) {
    if (file) file.close();
    return false;
  }
  file.flush();
  const bool synced = file.sync();
  const bool closed = file.close();
  bool verifiedHadBook = false;
  return synced && closed && readReplacementPending(path, bookPath, verifiedHadBook) && verifiedHadBook == hadBook;
}

bool resetNonEpubUserState(const std::string& bookPath, const std::string& cachePath) {
  if (cachePath.empty()) {
    return BookmarkUtil::writeEmptyCanonicalBookmark(bookPath);
  }
  if ((!Storage.exists(CACHE_ROOT) && !Storage.mkdir(CACHE_ROOT)) ||
      (!Storage.exists(cachePath.c_str()) && !Storage.mkdir(cachePath.c_str()))) {
    return false;
  }
  const bool clippingsHandled = cacheBackedBookKind(bookPath) != CacheBackedBookKind::Text ||
                                ClippingStore::quarantineFilesForBook(bookPath, cachePath, "txt");
  return clippingsHandled && BookmarkUtil::quarantineCanonicalForReplacement(bookPath, cachePath) &&
         resetBookCacheUserStateAfterReplacement(cachePath, bookPath);
}

enum class FileComparison : uint8_t { Equal, Different, IoError };

FileComparison compareFiles(const std::string& firstPath, const std::string& secondPath) {
  HalFile first;
  HalFile second;
  if (!Storage.openFileForRead("BookMove", firstPath, first) ||
      !Storage.openFileForRead("BookMove", secondPath, second)) {
    if (first) first.close();
    if (second) second.close();
    return FileComparison::IoError;
  }
  const auto closeBoth = [&] {
    const bool firstClosed = first.close();
    const bool secondClosed = second.close();
    return firstClosed && secondClosed;
  };
  if (first.fileSize64() != second.fileSize64()) {
    return closeBoth() ? FileComparison::Different : FileComparison::IoError;
  }

  std::array<uint8_t, 512> firstBytes{};
  std::array<uint8_t, 512> secondBytes{};
  while (first.available() > 0) {
    const size_t wanted = std::min(firstBytes.size(), static_cast<size_t>(first.available()));
    const int firstRead = first.read(firstBytes.data(), wanted);
    const int secondRead = second.read(secondBytes.data(), wanted);
    if (firstRead != static_cast<int>(wanted) || secondRead != static_cast<int>(wanted)) {
      first.close();
      second.close();
      return FileComparison::IoError;
    }
    if (!std::equal(firstBytes.begin(), firstBytes.begin() + wanted, secondBytes.begin())) {
      return closeBoth() ? FileComparison::Different : FileComparison::IoError;
    }
  }
  return closeBoth() ? FileComparison::Equal : FileComparison::IoError;
}

bool identifyEpub(const std::string& path, ZipFile::SourceIdentity& identity) {
  ZipFile source(path);
  return source.getSourceIdentity(identity);
}

bool cancelBarrierAfterFailedPublish(const std::string& cachePath) {
  if (SourceIdentityStore::cancelReplacement(cachePath)) return true;
  LOG_ERR("BookMove", "Could not cancel EPUB replacement barrier: %s", cachePath.c_str());
  return false;
}

bool quarantineInvalidPublishedBook(const std::string& bookPath) {
  constexpr unsigned MAX_SLOTS = 256;
  for (unsigned slot = 0; slot < MAX_SLOTS; ++slot) {
    std::string destination = hiddenBookFileSibling(bookPath, ".crossvi-invalid-publish");
    if (slot > 0) destination += "." + std::to_string(slot + 1);
    if (Storage.exists(destination.c_str())) continue;
    return Storage.rename(bookPath.c_str(), destination.c_str());
  }
  return false;
}

BookReplacementDisposition ensureReplacementBarrier(const std::string& bookPath, const std::string& cachePath) {
  ZipFile::SourceIdentity stored;
  const SourceIdentityStore::LoadStatus status = SourceIdentityStore::load(cachePath, stored);
  if (status == SourceIdentityStore::LoadStatus::NewerVersion || status == SourceIdentityStore::LoadStatus::Invalid ||
      status == SourceIdentityStore::LoadStatus::IoError) {
    return BookReplacementDisposition::Error;
  }

  const bool hasStoredIdentity = status == SourceIdentityStore::LoadStatus::Primary ||
                                 status == SourceIdentityStore::LoadStatus::Backup ||
                                 status == SourceIdentityStore::LoadStatus::Temp;
  if (hasStoredIdentity) {
    ZipFile source(bookPath);
    ZipFile::SourceIdentity current;
    // Upload handlers also call the reset hook after a byte-identical replace.
    // Preserve all state when the durable identity still matches. If the new
    // file is unreadable, continue quarantining the old state rather than
    // guessing that it is safe to retain.
    if (source.getSourceIdentity(current) && current == stored) return BookReplacementDisposition::SameSource;
  }
  if ((!Storage.exists(CACHE_ROOT) && !Storage.mkdir(CACHE_ROOT)) ||
      (!Storage.exists(cachePath.c_str()) && !Storage.mkdir(cachePath.c_str()))) {
    return BookReplacementDisposition::Error;
  }

  return SourceIdentityStore::prepareReplacement(cachePath, nullptr) ==
                 SourceIdentityStore::PrepareReplacementStatus::Prepared
             ? BookReplacementDisposition::Quarantine
             : BookReplacementDisposition::Error;
}

}  // namespace

std::string hiddenBookFileSibling(const std::string& bookPath, const char* suffix) {
  const size_t slash = bookPath.find_last_of('/');
  const size_t nameStart = slash == std::string::npos ? 0 : slash + 1;
  std::string path;
  path.reserve(bookPath.size() + 1 + std::char_traits<char>::length(suffix));
  path.append(bookPath, 0, nameStart);
  path += '.';
  path.append(bookPath, nameStart, std::string::npos);
  path += suffix;
  return path;
}

bool isBookFileTransactionArtifact(const char* fileName) {
  if (!fileName) return false;
  const std::string_view name(fileName);
  const auto endsWith = [name](const std::string_view suffix) {
    return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
  };
  return endsWith(".crossvi-upload.tmp") || endsWith(".crossvi-download.tmp") || endsWith(".crossvi-replace.bak") ||
         endsWith(".crossvi-replace.pending") || endsWith(".davtmp") ||
         name.find(".crossvi-invalid-publish") != std::string_view::npos;
}

bool canDeleteOrRelocateBookFile(const std::string& bookPath) {
  const CacheBackedBookKind kind = cacheBackedBookKind(bookPath);
  return !hasCompletionTrackedStats(kind) ||
         ReadingStatsCompletionTransaction::canRelocateOrDeleteBookCache(bookCachePath(bookPath));
}

bool recoverInterruptedBookFileReplacement(const std::string& bookPath) {
  if (!canDeleteOrRelocateBookFile(bookPath)) return false;
  const std::string oldBookPath = hiddenBookFileSibling(bookPath, REPLACED_BOOK_SUFFIX);
  const std::string pendingPath = hiddenBookFileSibling(bookPath, REPLACEMENT_PENDING_SUFFIX);
  const bool pending = Storage.exists(pendingPath.c_str());
  bool hadBook = false;
  if (pending && !readReplacementPending(pendingPath, bookPath, hadBook)) return false;
  const bool finalExists = Storage.exists(bookPath.c_str());
  const bool oldExists = Storage.exists(oldBookPath.c_str());
  // A backup-shaped sibling is never sufficient ownership proof. This also
  // prevents a copied/mismatched marker from authorizing deletion because the
  // marker payload is bound to the exact canonical book path.
  if (oldExists && (!pending || !hadBook)) return false;

  const bool isEpub = FsHelpers::hasEpubExtension(bookPath);
  if (!isEpub) {
    switch (decideNonEpubReplacementRecovery(pending, hadBook, finalExists, oldExists)) {
      case NonEpubReplacementRecovery::Nothing:
        return true;
      case NonEpubReplacementRecovery::CancelPending:
        return Storage.remove(pendingPath.c_str());
      case NonEpubReplacementRecovery::RestoreOld:
        if (!Storage.rename(oldBookPath.c_str(), bookPath.c_str())) return false;
        return !pending || Storage.remove(pendingPath.c_str());
      case NonEpubReplacementRecovery::FinalizeReplacement: {
        const std::string cachePath = bookCachePath(bookPath);
        if (!resetNonEpubUserState(bookPath, cachePath)) return false;
        // The marker must outlive the physical backup. A reset after this
        // removal can safely recognize final+pending+no-old as committed.
        if (oldExists && !Storage.remove(oldBookPath.c_str())) return false;
        return Storage.remove(pendingPath.c_str());
      }
      case NonEpubReplacementRecovery::RemoveOldBackup:
        return false;
      case NonEpubReplacementRecovery::FailClosed:
        return false;
    }
    return false;
  }

  if (!finalExists) {
    if (!oldExists) return !pending || Storage.remove(pendingPath.c_str());
    if (!Storage.rename(oldBookPath.c_str(), bookPath.c_str())) return false;
    ZipFile::SourceIdentity restoredIdentity;
    if (!identifyEpub(bookPath, restoredIdentity)) return false;
    const SourceIdentityStore::RecoverReplacementStatus recovered =
        SourceIdentityStore::recoverReplacement(bookCachePath(bookPath), restoredIdentity);
    if (recovered != SourceIdentityStore::RecoverReplacementStatus::RestoredCurrentSource &&
        recovered != SourceIdentityStore::RecoverReplacementStatus::NotPrepared) {
      return false;
    }
    return Storage.remove(pendingPath.c_str());
  }

  ZipFile::SourceIdentity current;
  if (!identifyEpub(bookPath, current)) {
    if (!oldExists) return true;
    ZipFile::SourceIdentity oldIdentity;
    if (!identifyEpub(oldBookPath, oldIdentity) || !quarantineInvalidPublishedBook(bookPath) ||
        !Storage.rename(oldBookPath.c_str(), bookPath.c_str())) {
      return false;
    }
    const std::string cachePath = bookCachePath(bookPath);
    const SourceIdentityStore::RecoverReplacementStatus recovered =
        SourceIdentityStore::recoverReplacement(cachePath, oldIdentity);
    if (recovered != SourceIdentityStore::RecoverReplacementStatus::RestoredCurrentSource &&
        recovered != SourceIdentityStore::RecoverReplacementStatus::NotPrepared) {
      return false;
    }
    return Storage.remove(pendingPath.c_str());
  }
  const std::string cachePath = bookCachePath(bookPath);
  switch (SourceIdentityStore::recoverReplacement(cachePath, current)) {
    case SourceIdentityStore::RecoverReplacementStatus::NotPrepared:
    case SourceIdentityStore::RecoverReplacementStatus::RestoredCurrentSource:
      break;
    case SourceIdentityStore::RecoverReplacementStatus::ReplacementPublished:
      if (!resetBookUserStateAfterReplacement(bookPath)) return false;
      break;
    case SourceIdentityStore::RecoverReplacementStatus::NewerVersion:
    case SourceIdentityStore::RecoverReplacementStatus::Invalid:
    case SourceIdentityStore::RecoverReplacementStatus::IoError:
      return false;
  }
  if (oldExists && !Storage.remove(oldBookPath.c_str())) {
    LOG_ERR("BookMove", "Could not remove published-book backup: %s", oldBookPath.c_str());
    return false;
  }
  return !pending || Storage.remove(pendingPath.c_str());
}

BookFilePublishResult publishStagedBookFile(const std::string& stagingPath, const std::string& bookPath) {
  if (stagingPath.empty() || stagingPath == bookPath || !Storage.exists(stagingPath.c_str())) {
    return BookFilePublishResult::StorageError;
  }
  if (!recoverInterruptedBookFileReplacement(bookPath)) return BookFilePublishResult::StateUnavailable;

  const std::string oldBookPath = hiddenBookFileSibling(bookPath, REPLACED_BOOK_SUFFIX);
  const std::string pendingPath = hiddenBookFileSibling(bookPath, REPLACEMENT_PENDING_SUFFIX);
  if (Storage.exists(oldBookPath.c_str()) || Storage.exists(pendingPath.c_str())) {
    return BookFilePublishResult::StateUnavailable;
  }

  const bool isEpub = FsHelpers::hasEpubExtension(bookPath);
  ZipFile::SourceIdentity stagedIdentity;
  if (isEpub && !identifyEpub(stagingPath, stagedIdentity)) return BookFilePublishResult::InvalidStagedFile;
  if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc stagedBook(stagingPath, CACHE_ROOT);
    if (!stagedBook.load()) return BookFilePublishResult::InvalidStagedFile;
  }
  if (cacheBackedBookKind(bookPath) == CacheBackedBookKind::Text) {
    // Validate with the same complete-file scan used by Reader. In particular,
    // never replace a working book with a staged file that is unreadable or
    // too large for TXT's uint32 index/progress format.
    Txt stagedBook(stagingPath, CACHE_ROOT);
    if (!stagedBook.load()) return BookFilePublishResult::InvalidStagedFile;
  }

  const bool hadBook = Storage.exists(bookPath.c_str());
  if (hadBook) {
    switch (compareFiles(stagingPath, bookPath)) {
      case FileComparison::Equal:
        return Storage.remove(stagingPath.c_str()) ? BookFilePublishResult::Unchanged
                                                   : BookFilePublishResult::StorageError;
      case FileComparison::Different:
        break;
      case FileComparison::IoError:
        return BookFilePublishResult::StorageError;
    }
  }

  const std::string cachePath = bookCachePath(bookPath);
  const bool hasState = !cachePath.empty() && hasBookUserState(bookPath, cachePath);
  bool barrierPrepared = false;
  bool replacementPending = false;
  if (hasState) {
    if (isEpub) {
      ZipFile::SourceIdentity oldIdentity;
      const ZipFile::SourceIdentity* oldIdentityPtr = nullptr;
      if (hadBook) {
        if (!identifyEpub(bookPath, oldIdentity)) return BookFilePublishResult::StateUnavailable;
        oldIdentityPtr = &oldIdentity;
      }
      if (SourceIdentityStore::prepareReplacement(cachePath, oldIdentityPtr) !=
          SourceIdentityStore::PrepareReplacementStatus::Prepared) {
        return BookFilePublishResult::StateUnavailable;
      }
      barrierPrepared = true;
    }
  }

  // This path-bound marker is the ownership proof for the physical backup and
  // therefore must be durable before the old book is renamed. Non-EPUB orphan
  // state also needs it to distinguish publication from an interrupted upload.
  if (hadBook || (!isEpub && hasState)) {
    if (!writeReplacementPending(pendingPath, bookPath, hadBook)) {
      if (barrierPrepared) cancelBarrierAfterFailedPublish(cachePath);
      return BookFilePublishResult::StateUnavailable;
    }
    replacementPending = true;
  }

  if (hadBook && !Storage.rename(bookPath.c_str(), oldBookPath.c_str())) {
    if (barrierPrepared) cancelBarrierAfterFailedPublish(cachePath);
    if (replacementPending) Storage.remove(pendingPath.c_str());
    return BookFilePublishResult::StorageError;
  }
  if (!Storage.rename(stagingPath.c_str(), bookPath.c_str())) {
    const bool restored = !hadBook || Storage.rename(oldBookPath.c_str(), bookPath.c_str());
    if (barrierPrepared && restored) cancelBarrierAfterFailedPublish(cachePath);
    if (replacementPending && restored) Storage.remove(pendingPath.c_str());
    return BookFilePublishResult::StorageError;
  }

  if (isEpub) {
    ZipFile::SourceIdentity publishedIdentity;
    if (!identifyEpub(bookPath, publishedIdentity) || publishedIdentity != stagedIdentity) {
      // Move the damaged/unverified publication back out of the authoritative
      // path before restoring and unblocking old state. If that cannot be done,
      // retain the barrier and old-book backup so Reader fails closed.
      const bool movedAside = Storage.rename(bookPath.c_str(), stagingPath.c_str());
      const bool restored = movedAside && (!hadBook || Storage.rename(oldBookPath.c_str(), bookPath.c_str()));
      if (barrierPrepared && restored) cancelBarrierAfterFailedPublish(cachePath);
      return BookFilePublishResult::StorageError;
    }
  }

  if (barrierPrepared && !resetBookUserStateAfterReplacement(bookPath)) {
    return BookFilePublishResult::StateUnavailable;
  }
  if (!isEpub && hasState && !resetNonEpubUserState(bookPath, cachePath)) {
    return BookFilePublishResult::StateUnavailable;
  }
  if (hadBook && Storage.exists(oldBookPath.c_str()) && !Storage.remove(oldBookPath.c_str())) {
    LOG_ERR("BookMove", "Could not remove published-book backup: %s", oldBookPath.c_str());
    return BookFilePublishResult::StateUnavailable;
  }
  if (replacementPending && !Storage.remove(pendingPath.c_str())) return BookFilePublishResult::StateUnavailable;
  return BookFilePublishResult::Published;
}

BookPathMoveResult moveBookFilePreservingUserState(const std::string& sourcePath, const std::string& destinationPath) {
  if (!Storage.exists(sourcePath.c_str())) return BookPathMoveResult::SourceMissing;
  if (Storage.exists(destinationPath.c_str())) return BookPathMoveResult::DestinationExists;

  const CacheBackedBookKind sourceKind = cacheBackedBookKind(sourcePath);
  const CacheBackedBookKind destinationKind = cacheBackedBookKind(destinationPath);
  if (sourceKind == CacheBackedBookKind::None && destinationKind == CacheBackedBookKind::None) {
    if (!Storage.rename(sourcePath.c_str(), destinationPath.c_str())) return BookPathMoveResult::StorageError;
    return BookPathMoveResult::Moved;
  }
  // Moving into or out of a different reader format cannot safely translate
  // progress/layout state. Refuse instead of deleting it or letting the new
  // path inherit unrelated state.
  if (sourceKind == CacheBackedBookKind::None || sourceKind != destinationKind) {
    return BookPathMoveResult::StateUnavailable;
  }

  const std::string sourceCachePath = bookCachePath(sourcePath);
  const std::string destinationCachePath = bookCachePath(destinationPath);
  if (hasCompletionTrackedStats(sourceKind) &&
      (!ReadingStatsCompletionTransaction::canRelocateOrDeleteBookCache(sourceCachePath) ||
       !ReadingStatsCompletionTransaction::canRelocateOrDeleteBookCache(destinationCachePath))) {
    return BookPathMoveResult::StateUnavailable;
  }
  if (!prepareBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath)) {
    return BookPathMoveResult::StateUnavailable;
  }

  if (sourceKind == CacheBackedBookKind::Text) {
    ClippingStore clippings;
    const bool hasClippings = ClippingStore::hasFilesForBook(sourcePath, "txt");
    ClippingStore::RekeyResult preparedClippings = ClippingStore::RekeyResult::Unchanged;
    if (hasClippings) {
      const ClippingStore::LoadResult load = clippings.loadForBook(sourcePath, "", "", "txt");
      if (!clippings.isLoaded()) {
        LOG_ERR("BookMove", "Could not inspect TXT clipping state before move (%u)", static_cast<unsigned>(load));
        cancelBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
        return BookPathMoveResult::StateUnavailable;
      }
      const auto sourceBook = clippings.book();
      preparedClippings =
          clippings.prepareRekeyForBook(destinationPath, sourceBook.title, sourceBook.author, "txt");
      if (preparedClippings != ClippingStore::RekeyResult::Prepared &&
          preparedClippings != ClippingStore::RekeyResult::Unchanged) {
        cancelBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
        return BookPathMoveResult::StateUnavailable;
      }
    }

    if (!Storage.rename(sourcePath.c_str(), destinationPath.c_str())) {
      if (hasClippings) clippings.cancelPreparedRekey();
      cancelBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
      return BookPathMoveResult::StorageError;
    }
    const bool cachePublished =
        finalizeBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
    if (!cachePublished && Storage.rename(destinationPath.c_str(), sourcePath.c_str())) {
      if (hasClippings) clippings.cancelPreparedRekey();
      cancelBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
      return BookPathMoveResult::StateUnavailable;
    }

    if (hasClippings) {
      const ClippingStore::RekeyResult finalized =
          preparedClippings == ClippingStore::RekeyResult::Unchanged ? ClippingStore::RekeyResult::Unchanged
                                                                     : clippings.finalizePreparedRekey();
      if (finalized != ClippingStore::RekeyResult::Rekeyed && finalized != ClippingStore::RekeyResult::Unchanged) {
        LOG_ERR("BookMove", "Could not finalize moved TXT clipping state (%u)", static_cast<unsigned>(finalized));
        if (Storage.rename(destinationPath.c_str(), sourcePath.c_str())) {
          if (cachePublished) {
            discardPublishedBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath,
                                                   destinationPath);
          } else {
            cancelBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
          }
          clippings.cancelPreparedRekey();
          return BookPathMoveResult::StateUnavailable;
        }
      }
    }
    if (cachePublished &&
        !completeBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath)) {
      LOG_ERR("BookMove", "Could not finish old TXT state cleanup");
    }
    RECENT_BOOKS.updatePath(sourcePath, destinationPath, sourceCachePath, destinationCachePath);
    if (APP_STATE.openEpubPath == sourcePath) {
      APP_STATE.openEpubPath = destinationPath;
      APP_STATE.saveToFile();
    }
    return BookPathMoveResult::Moved;
  }

  if (sourceKind != CacheBackedBookKind::Epub) {
    if (!Storage.rename(sourcePath.c_str(), destinationPath.c_str())) {
      cancelBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
      return BookPathMoveResult::StorageError;
    }

    const bool cachePublished =
        finalizeBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
    if (!cachePublished) {
      LOG_ERR("BookMove", "Could not publish moved non-EPUB state");
      if (Storage.rename(destinationPath.c_str(), sourcePath.c_str())) {
        cancelBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
        return BookPathMoveResult::StateUnavailable;
      }
      // The destination book is authoritative. Its reader will retry the
      // verified staging transaction before loading any path-keyed state.
      LOG_ERR("BookMove", "Could not roll back non-EPUB book after state publication failure");
    } else if (!completeBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath)) {
      LOG_ERR("BookMove", "Could not finish old non-EPUB state cleanup");
    }

    RECENT_BOOKS.updatePath(sourcePath, destinationPath, sourceCachePath, destinationCachePath);
    if (APP_STATE.openEpubPath == sourcePath) {
      APP_STATE.openEpubPath = destinationPath;
      APP_STATE.saveToFile();
    }
    return BookPathMoveResult::Moved;
  }

  ClippingStore clippings;
  if (!clippings.isLoaded()) {
    const ClippingStore::LoadResult load = clippings.loadForBook(sourcePath, "", "");
    if (!clippings.isLoaded()) {
      LOG_ERR("BookMove", "Could not inspect clipping state before move (%u)", static_cast<unsigned>(load));
      cancelBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
      return BookPathMoveResult::StateUnavailable;
    }
  }
  const ClippingCodec::BookMetadata sourceBook = clippings.book();
  const ClippingStore::RekeyResult prepared =
      clippings.prepareRekeyForBook(destinationPath, sourceBook.title, sourceBook.author, sourceBook.bookType);
  if (prepared != ClippingStore::RekeyResult::Prepared && prepared != ClippingStore::RekeyResult::Unchanged) {
    cancelBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
    return BookPathMoveResult::StateUnavailable;
  }

  if (!Storage.rename(sourcePath.c_str(), destinationPath.c_str())) {
    clippings.cancelPreparedRekey();
    cancelBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
    return BookPathMoveResult::StorageError;
  }

  const bool cachePublished =
      finalizeBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
  if (!cachePublished) {
    LOG_ERR("BookMove", "Could not publish moved cache state");
    if (Storage.rename(destinationPath.c_str(), sourcePath.c_str())) {
      clippings.cancelPreparedRekey();
      cancelBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
      return BookPathMoveResult::StateUnavailable;
    }
    // The destination book is now authoritative. Keep staging for Reader
    // recovery and continue finalizing every independently verified state.
    LOG_ERR("BookMove", "Could not roll back book after cache publication failure");
  }

  const ClippingStore::RekeyResult finalized = prepared == ClippingStore::RekeyResult::Unchanged
                                                   ? ClippingStore::RekeyResult::Unchanged
                                                   : clippings.finalizePreparedRekey();
  if (finalized != ClippingStore::RekeyResult::Rekeyed && finalized != ClippingStore::RekeyResult::Unchanged) {
    LOG_ERR("BookMove", "Could not finalize moved clipping state (%u)", static_cast<unsigned>(finalized));
    if (Storage.rename(destinationPath.c_str(), sourcePath.c_str())) {
      if (cachePublished) {
        discardPublishedBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
      } else {
        cancelBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath);
      }
      clippings.cancelPreparedRekey();
      return BookPathMoveResult::StateUnavailable;
    }
    LOG_ERR("BookMove", "Could not roll back book after clipping publication failure");
  }

  if (cachePublished) {
    if (!completeBookCacheUserStateMove(sourceCachePath, destinationCachePath, sourcePath, destinationPath)) {
      LOG_ERR("BookMove", "Could not remove old bookmark duplicate after move");
    }
  }
  RECENT_BOOKS.updatePath(sourcePath, destinationPath, sourceCachePath, destinationCachePath);
  if (APP_STATE.openEpubPath == sourcePath) {
    APP_STATE.openEpubPath = destinationPath;
    APP_STATE.saveToFile();
  }
  return BookPathMoveResult::Moved;
}

bool removeBookUserStateAfterDelete(const std::string& bookPath) {
  const CacheBackedBookKind kind = cacheBackedBookKind(bookPath);
  if (kind != CacheBackedBookKind::Epub) {
    const std::string cachePath = bookCachePath(bookPath);
    if (kind == CacheBackedBookKind::Text &&
        !ReadingStatsCompletionTransaction::canRelocateOrDeleteBookCache(cachePath)) {
      return false;
    }
    if (!cachePath.empty()) return resetNonEpubUserState(bookPath, cachePath);
    const std::string bookmarkPath = BookmarkUtil::getBookmarkPath(bookPath);
    const bool bookmarkRemoved = removeIfPresent(bookmarkPath);
    if (!bookmarkRemoved) logCleanupFailure("bookmark", bookmarkPath);
    return bookmarkRemoved;
  }

  const std::string cachePath = Epub(bookPath, CACHE_ROOT).getCachePath();
  if (!ReadingStatsCompletionTransaction::canRelocateOrDeleteBookCache(cachePath)) return false;
  // Validate/remove external clipping state first and quarantine the cache
  // last. If an earlier step fails, the old source-identity sidecar remains in
  // the canonical cache so every future open continues to fail closed.
  const bool clippingsRemoved = ClippingStore::removeFilesForBook(bookPath);
  if (!clippingsRemoved) logCleanupFailure("clipping", bookPath);

  // Deletion/replacement makes every canonical bookmark at this path stale.
  // A verified empty canonical also prevents an ambiguous legacy key from
  // resurfacing for a future book copied to the same path.
  const bool bookmarkReset = BookmarkUtil::writeEmptyCanonicalBookmark(bookPath);
  if (!bookmarkReset) logCleanupFailure("bookmark tombstone", bookPath);

  bool cacheReset = false;
  if (clippingsRemoved && bookmarkReset) {
    cacheReset = resetBookCacheUserStateAfterReplacement(cachePath, bookPath);
    if (!cacheReset) logCleanupFailure("cache transaction", bookPath);
  }
  return clippingsRemoved && bookmarkReset && cacheReset;
}

bool resetBookUserStateAfterReplacement(const std::string& bookPath) {
  const CacheBackedBookKind kind = cacheBackedBookKind(bookPath);
  if (kind != CacheBackedBookKind::Epub) {
    const std::string cachePath = bookCachePath(bookPath);
    if (kind == CacheBackedBookKind::Text &&
        !ReadingStatsCompletionTransaction::canRelocateOrDeleteBookCache(cachePath)) {
      return false;
    }
    return cachePath.empty() ? removeBookUserStateAfterDelete(bookPath) : resetNonEpubUserState(bookPath, cachePath);
  }

  const std::string cachePath = bookCachePath(bookPath);
  if (!ReadingStatsCompletionTransaction::canRelocateOrDeleteBookCache(cachePath)) return false;
  const bool hasState = hasBookUserState(bookPath, cachePath);
  // Establish a durable fail-closed barrier first, including for legacy paths
  // that had external state but no cache yet. A reset at any later boundary
  // retries this same transaction instead of adopting the replacement.
  const BookReplacementDisposition disposition =
      hasState ? ensureReplacementBarrier(bookPath, cachePath) : BookReplacementDisposition::NoState;
  return runBookReplacementTransaction(
      disposition, [&] { return ClippingStore::quarantineFilesForBook(bookPath, cachePath); },
      [&] { return BookmarkUtil::quarantineCanonicalForReplacement(bookPath, cachePath); },
      // This resolves owned clear/move artifacts and renames the canonical
      // cache last. It returns immediately after that atomic commit.
      [&] { return resetBookCacheUserStateAfterReplacement(cachePath, bookPath); });
}
