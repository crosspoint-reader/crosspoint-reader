#include "BookCacheUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "BookmarkUtil.h"

namespace {

constexpr char CLEAR_STAGING_PREFIX[] = ".crossvi_clear_";
constexpr char MOVE_STAGING_PREFIX[] = ".crossvi_move_";
constexpr char MOVE_CLEANUP_PREFIX[] = ".crossvi_cleanup_";
constexpr char REPLACEMENT_DISCARD_PREFIX[] = ".crossvi_discard_";
constexpr char MOVE_READY_MARKER[] = ".crossvi_move_ready";
constexpr char MOVE_BOOKMARK_PAYLOAD[] = ".crossvi_bookmark.json";
constexpr std::array<uint8_t, 5> MOVE_READY_MAGIC = {'C', 'V', 'M', 'S', '2'};
constexpr size_t MOVE_READY_HEADER_SIZE = MOVE_READY_MAGIC.size() + (sizeof(uint16_t) * 4);
constexpr size_t MAX_MOVE_PATH_BYTES = 512;
constexpr char VERSIONED_STATS_PREFIX[] = "stats_v";
constexpr std::array<const char*, 3> VERSIONED_STATS_SUFFIXES = {".bin", ".bin.bak", ".bin.tmp"};
constexpr std::array<const char*, 3> READER_SETTINGS_FILES = {
    "crossvi_reader_settings.bin", "crossvi_reader_settings.bin.bak", "crossvi_reader_settings.bin.tmp"};

struct CacheEntry {
  std::string name;
  bool directory = false;
};

struct MoveIdentity {
  std::string sourceBookPath;
  std::string destinationBookPath;
  std::string sourceCachePath;
  std::string destinationCachePath;
};

bool operator==(const MoveIdentity& left, const MoveIdentity& right) {
  return left.sourceBookPath == right.sourceBookPath && left.destinationBookPath == right.destinationBookPath &&
         left.sourceCachePath == right.sourceCachePath && left.destinationCachePath == right.destinationCachePath;
}

bool isDirectory(const std::string& path) {
  HalFile file = Storage.open(path.c_str());
  if (!file) return false;
  const bool result = file.isDirectory();
  file.close();
  return result;
}

bool readDirectory(const std::string& path, std::vector<CacheEntry>& entries) {
  entries.clear();
  HalFile directory = Storage.open(path.c_str());
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return false;
  }

  char name[256]{};
  for (HalFile file = directory.openNextFile(); file; file = directory.openNextFile()) {
    const bool entryIsDirectory = file.isDirectory();
    const size_t nameLength = file.getName(name, sizeof(name));
    file.close();
    if (nameLength == 0 || nameLength >= sizeof(name)) {
      directory.close();
      return false;
    }

    std::string entryName(name, nameLength);
    if (entryName == "." || entryName == ".." || entryName.find('/') != std::string::npos ||
        entryName.find('\\') != std::string::npos) {
      directory.close();
      return false;
    }
    entries.push_back({std::move(entryName), entryIsDirectory});
  }
  return directory.close();
}

bool isVersionedStatsFileName(const std::string& name) {
  constexpr size_t prefixLength = sizeof(VERSIONED_STATS_PREFIX) - 1;
  if (name.compare(0, prefixLength, VERSIONED_STATS_PREFIX) != 0) return false;

  for (const char* suffix : VERSIONED_STATS_SUFFIXES) {
    const size_t suffixLength = strlen(suffix);
    if (name.size() <= prefixLength + suffixLength ||
        name.compare(name.size() - suffixLength, suffixLength, suffix) != 0) {
      continue;
    }
    return std::all_of(name.begin() + prefixLength, name.end() - suffixLength,
                       [](const char character) { return character >= '0' && character <= '9'; });
  }
  return false;
}

bool isUserStateFileName(const std::string& name) {
  if (name == "stats.bin" || isVersionedStatsFileName(name)) return true;
  return std::any_of(READER_SETTINGS_FILES.begin(), READER_SETTINGS_FILES.end(),
                     [&name](const char* candidate) { return name == candidate; });
}

bool isMovableUserStateFileName(const std::string& name) {
  return isUserStateFileName(name) || name == "progress.bin" || name == "progress.bin.tmp";
}

bool isMoveStagingFileName(const std::string& name) {
  return name == MOVE_READY_MARKER || name == MOVE_BOOKMARK_PAYLOAD || isMovableUserStateFileName(name);
}

std::string childPath(const std::string& directory, const std::string& name) {
  return directory + (directory.empty() || directory.back() == '/' ? "" : "/") + name;
}

std::string sourceBookmarkPathFor(const std::string& bookPath) {
  const std::string canonical = BookmarkUtil::getBookmarkPath(bookPath);
  return Storage.exists(canonical.c_str()) ? canonical : BookmarkUtil::getLegacyBookmarkPath(bookPath);
}

bool destinationNeedsEmptyBookmarkPayload(const MoveIdentity& identity) {
  const std::string sourceBookmark = sourceBookmarkPathFor(identity.sourceBookPath);
  return !Storage.exists(sourceBookmark.c_str()) &&
         Storage.exists(BookmarkUtil::getLegacyBookmarkPath(identity.destinationBookPath).c_str());
}

std::string normalizedCachePath(std::string path) {
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  return path;
}

std::string stagingPathFor(const std::string& cachePath) {
  const size_t separator = cachePath.rfind('/');
  const size_t nameOffset = separator == std::string::npos ? 0 : separator + 1;
  if (nameOffset >= cachePath.size()) return {};
  return cachePath.substr(0, nameOffset) + CLEAR_STAGING_PREFIX + cachePath.substr(nameOffset);
}

std::string moveStagingPathFor(const std::string& cachePath) {
  const size_t separator = cachePath.rfind('/');
  const size_t nameOffset = separator == std::string::npos ? 0 : separator + 1;
  if (nameOffset >= cachePath.size()) return {};
  return cachePath.substr(0, nameOffset) + MOVE_STAGING_PREFIX + cachePath.substr(nameOffset);
}

std::string moveCleanupPathFor(const std::string& cachePath) {
  const size_t separator = cachePath.rfind('/');
  const size_t nameOffset = separator == std::string::npos ? 0 : separator + 1;
  if (nameOffset >= cachePath.size()) return {};
  return cachePath.substr(0, nameOffset) + MOVE_CLEANUP_PREFIX + cachePath.substr(nameOffset);
}

bool filesEqual(const std::string& leftPath, const std::string& rightPath) {
  HalFile left;
  HalFile right;
  if (!Storage.openFileForRead("BookCache", leftPath, left) ||
      !Storage.openFileForRead("BookCache", rightPath, right) || left.fileSize64() != right.fileSize64()) {
    return false;
  }

  std::array<uint8_t, 512> leftBuffer{};
  std::array<uint8_t, 512> rightBuffer{};
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

bool copyFileExact(const std::string& sourcePath, const std::string& destinationPath) {
  if (Storage.exists(destinationPath.c_str())) return false;
  HalFile source;
  HalFile destination;
  if (!Storage.openFileForRead("BookCache", sourcePath, source) ||
      !Storage.openFileForWrite("BookCache", destinationPath, destination)) {
    return false;
  }

  std::array<uint8_t, 512> buffer{};
  uint64_t remaining = source.fileSize64();
  bool copied = true;
  while (remaining > 0) {
    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining));
    if (source.read(buffer.data(), chunk) != static_cast<int>(chunk) ||
        destination.write(buffer.data(), chunk) != chunk) {
      copied = false;
      break;
    }
    remaining -= chunk;
  }
  destination.flush();
  copied = copied && destination.sync() && destination.close();
  source.close();
  if (!copied || !filesEqual(sourcePath, destinationPath)) {
    Storage.remove(destinationPath.c_str());
    return false;
  }
  return true;
}

void encodeUint16(const uint16_t value, uint8_t* output) {
  output[0] = static_cast<uint8_t>(value & 0xFFU);
  output[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
}

uint16_t decodeUint16(const uint8_t* input) {
  return static_cast<uint16_t>(input[0]) | (static_cast<uint16_t>(input[1]) << 8U);
}

bool validMoveIdentity(const MoveIdentity& identity) {
  const std::array<const std::string*, 4> paths = {&identity.sourceBookPath, &identity.destinationBookPath,
                                                   &identity.sourceCachePath, &identity.destinationCachePath};
  return identity.sourceBookPath != identity.destinationBookPath &&
         identity.sourceCachePath != identity.destinationCachePath &&
         std::all_of(paths.begin(), paths.end(), [](const std::string* path) {
           return path && !path->empty() && *path != "/" && path->size() <= MAX_MOVE_PATH_BYTES;
         });
}

std::vector<uint8_t> encodeMoveIdentity(const MoveIdentity& identity) {
  if (!validMoveIdentity(identity)) return {};
  const std::array<const std::string*, 4> paths = {&identity.sourceBookPath, &identity.destinationBookPath,
                                                   &identity.sourceCachePath, &identity.destinationCachePath};
  size_t totalSize = MOVE_READY_HEADER_SIZE;
  for (const std::string* path : paths) totalSize += path->size();

  std::vector<uint8_t> encoded(totalSize);
  std::copy(MOVE_READY_MAGIC.begin(), MOVE_READY_MAGIC.end(), encoded.begin());
  size_t headerOffset = MOVE_READY_MAGIC.size();
  size_t payloadOffset = MOVE_READY_HEADER_SIZE;
  for (const std::string* path : paths) {
    encodeUint16(static_cast<uint16_t>(path->size()), encoded.data() + headerOffset);
    headerOffset += sizeof(uint16_t);
    std::copy(path->begin(), path->end(), encoded.begin() + static_cast<std::ptrdiff_t>(payloadOffset));
    payloadOffset += path->size();
  }
  return encoded;
}

bool readMoveIdentity(const std::string& stagingPath, MoveIdentity& identity) {
  const std::string markerPath = childPath(stagingPath, MOVE_READY_MARKER);
  HalFile marker;
  if (!Storage.openFileForRead("BookCache", markerPath, marker)) return false;
  const uint64_t fileSize = marker.fileSize64();
  if (fileSize < MOVE_READY_HEADER_SIZE || fileSize > MOVE_READY_HEADER_SIZE + (MAX_MOVE_PATH_BYTES * 4)) {
    return false;
  }

  std::array<uint8_t, MOVE_READY_HEADER_SIZE> header{};
  if (marker.read(header.data(), header.size()) != static_cast<int>(header.size()) ||
      !std::equal(MOVE_READY_MAGIC.begin(), MOVE_READY_MAGIC.end(), header.begin())) {
    return false;
  }

  std::array<uint16_t, 4> lengths{};
  size_t expectedSize = MOVE_READY_HEADER_SIZE;
  for (size_t i = 0; i < lengths.size(); ++i) {
    lengths[i] = decodeUint16(header.data() + MOVE_READY_MAGIC.size() + (i * sizeof(uint16_t)));
    if (lengths[i] == 0 || lengths[i] > MAX_MOVE_PATH_BYTES) return false;
    expectedSize += lengths[i];
  }
  if (fileSize != expectedSize) return false;

  std::array<std::string*, 4> paths = {&identity.sourceBookPath, &identity.destinationBookPath,
                                       &identity.sourceCachePath, &identity.destinationCachePath};
  for (size_t i = 0; i < paths.size(); ++i) {
    paths[i]->resize(lengths[i]);
    if (marker.read(paths[i]->data(), lengths[i]) != static_cast<int>(lengths[i])) return false;
  }
  return validMoveIdentity(identity);
}

bool writeMoveReadyMarker(const std::string& stagingPath, const MoveIdentity& identity) {
  const std::vector<uint8_t> encoded = encodeMoveIdentity(identity);
  if (encoded.empty()) return false;

  const std::string markerPath = childPath(stagingPath, MOVE_READY_MARKER);
  HalFile marker;
  if (!Storage.openFileForWrite("BookCache", markerPath, marker)) return false;
  const bool written = marker.write(encoded.data(), encoded.size()) == encoded.size();
  marker.flush();
  if (!written || !marker.sync() || !marker.close()) return false;

  MoveIdentity verified;
  return readMoveIdentity(stagingPath, verified) && verified == identity;
}

bool validateMoveStaging(const std::string& stagingPath, const MoveIdentity& expectedIdentity) {
  MoveIdentity storedIdentity;
  if (!readMoveIdentity(stagingPath, storedIdentity) || !(storedIdentity == expectedIdentity)) return false;

  std::vector<CacheEntry> stagedEntries;
  if (!readDirectory(stagingPath, stagedEntries)) return false;
  std::vector<std::string> stagedNames;
  bool stagedBookmark = false;
  for (const CacheEntry& entry : stagedEntries) {
    if (entry.directory || !isMoveStagingFileName(entry.name)) return false;
    if (entry.name == MOVE_BOOKMARK_PAYLOAD) {
      stagedBookmark = true;
    } else if (entry.name != MOVE_READY_MARKER) {
      stagedNames.push_back(entry.name);
    }
  }
  std::sort(stagedNames.begin(), stagedNames.end());

  const std::string sourceBookmarkPath = sourceBookmarkPathFor(expectedIdentity.sourceBookPath);
  const bool sourceBookmark = Storage.exists(sourceBookmarkPath.c_str());
  const bool emptyDestinationPayload = !sourceBookmark && destinationNeedsEmptyBookmarkPayload(expectedIdentity);
  const std::string stagedBookmarkPath = childPath(stagingPath, MOVE_BOOKMARK_PAYLOAD);
  if ((sourceBookmark || emptyDestinationPayload) != stagedBookmark ||
      (sourceBookmark && !filesEqual(sourceBookmarkPath, stagedBookmarkPath)) ||
      (emptyDestinationPayload && !BookmarkUtil::isEmptyBookmarkFile(stagedBookmarkPath))) {
    return false;
  }

  if (!Storage.exists(expectedIdentity.sourceCachePath.c_str())) return stagedNames.empty();
  std::vector<CacheEntry> sourceEntries;
  if (!readDirectory(expectedIdentity.sourceCachePath, sourceEntries)) return false;
  std::vector<std::string> sourceNames;
  for (const CacheEntry& entry : sourceEntries) {
    if (!entry.directory && isMovableUserStateFileName(entry.name)) sourceNames.push_back(entry.name);
  }
  std::sort(sourceNames.begin(), sourceNames.end());
  if (sourceNames != stagedNames) return false;
  return std::all_of(sourceNames.begin(), sourceNames.end(), [&](const std::string& name) {
    return filesEqual(childPath(expectedIdentity.sourceCachePath, name), childPath(stagingPath, name));
  });
}

bool canDiscardInterruptedMoveStaging(const std::string& stagingPath, const MoveIdentity& identity) {
  std::vector<CacheEntry> entries;
  if (!readDirectory(stagingPath, entries)) return false;
  for (const CacheEntry& entry : entries) {
    if (entry.directory || entry.name == MOVE_READY_MARKER ||
        (entry.name != MOVE_BOOKMARK_PAYLOAD && !isMovableUserStateFileName(entry.name))) {
      return false;
    }
    const std::string stagedPath = childPath(stagingPath, entry.name);
    if (entry.name == MOVE_BOOKMARK_PAYLOAD) {
      const std::string sourcePath = sourceBookmarkPathFor(identity.sourceBookPath);
      if (Storage.exists(sourcePath.c_str())) {
        if (!filesEqual(sourcePath, stagedPath)) return false;
      } else if (!destinationNeedsEmptyBookmarkPayload(identity) || !BookmarkUtil::isEmptyBookmarkFile(stagedPath)) {
        return false;
      }
    } else {
      const std::string sourcePath = childPath(identity.sourceCachePath, entry.name);
      if (!Storage.exists(sourcePath.c_str()) || !filesEqual(sourcePath, stagedPath)) return false;
    }
  }
  return true;
}

bool isNonAuthoritativePreparedMove(const std::string& stagingPath, const MoveIdentity& identity) {
  MoveIdentity storedIdentity;
  if (!readMoveIdentity(stagingPath, storedIdentity) || !(storedIdentity == identity)) return false;

  std::vector<CacheEntry> entries;
  if (!readDirectory(stagingPath, entries)) return false;
  return std::all_of(entries.begin(), entries.end(),
                     [](const CacheEntry& entry) { return !entry.directory && isMoveStagingFileName(entry.name); });
}

bool ensureBookmarkDirectory() {
  constexpr char ROOT[] = "/.crosspoint";
  const std::string directory = BookmarkUtil::getBookmarksDir();
  return (Storage.exists(ROOT) || Storage.mkdir(ROOT)) &&
         (Storage.exists(directory.c_str()) || Storage.mkdir(directory.c_str()));
}

bool publishStagedBookmark(const std::string& stagingPath, const MoveIdentity& identity) {
  const std::string payloadPath = childPath(stagingPath, MOVE_BOOKMARK_PAYLOAD);
  const std::string destinationPath = BookmarkUtil::getBookmarkPath(identity.destinationBookPath);
  const bool payloadExists = Storage.exists(payloadPath.c_str());
  if (Storage.exists(destinationPath.c_str())) {
    if (payloadExists && filesEqual(payloadPath, destinationPath)) return true;
    // Only a verified empty tombstone may be replaced or reused. Non-empty,
    // malformed, or unreadable destination bookmarks remain protected.
    if (!BookmarkUtil::isEmptyBookmarkFile(destinationPath)) return false;
    if (!payloadExists || BookmarkUtil::isEmptyBookmarkFile(payloadPath)) return true;
    if (!Storage.remove(destinationPath.c_str())) return false;
  }
  return !payloadExists || (ensureBookmarkDirectory() && copyFileExact(payloadPath, destinationPath));
}

bool removePreparedBookmarkCopy(const std::string& stagingPath, const MoveIdentity& identity) {
  const std::string payloadPath = childPath(stagingPath, MOVE_BOOKMARK_PAYLOAD);
  const std::string destinationPath = BookmarkUtil::getBookmarkPath(identity.destinationBookPath);
  if (!Storage.exists(destinationPath.c_str())) return true;
  // An empty tombstone is safe and may have predated this transaction. Never
  // delete it during rollback merely because the staged payload is also empty.
  if (BookmarkUtil::isEmptyBookmarkFile(destinationPath)) return true;
  return Storage.exists(payloadPath.c_str()) && filesEqual(payloadPath, destinationPath) &&
         Storage.remove(destinationPath.c_str());
}

bool moveBoundaryCrossed(const MoveIdentity& identity) {
  return !Storage.exists(identity.sourceBookPath.c_str()) && Storage.exists(identity.destinationBookPath.c_str());
}

bool mergePreparedMoveIntoDerivedCache(const std::string& stagingPath, const MoveIdentity& identity) {
  if (!isDirectory(identity.destinationCachePath)) return false;

  std::vector<CacheEntry> destinationEntries;
  if (!readDirectory(identity.destinationCachePath, destinationEntries)) return false;
  for (const CacheEntry& entry : destinationEntries) {
    if (entry.directory || !isMovableUserStateFileName(entry.name)) continue;
    const std::string stagedPath = childPath(stagingPath, entry.name);
    const std::string destinationPath = childPath(identity.destinationCachePath, entry.name);
    if (!Storage.exists(stagedPath.c_str()) || !filesEqual(stagedPath, destinationPath)) return false;
  }

  std::vector<CacheEntry> stagedEntries;
  if (!readDirectory(stagingPath, stagedEntries)) return false;
  for (const CacheEntry& entry : stagedEntries) {
    if (entry.directory || !isMoveStagingFileName(entry.name)) return false;
    const std::string sourcePath = childPath(stagingPath, entry.name);
    const std::string destinationPath = childPath(identity.destinationCachePath, entry.name);
    if (Storage.exists(destinationPath.c_str())) {
      if (!filesEqual(sourcePath, destinationPath)) return false;
    } else if (!copyFileExact(sourcePath, destinationPath)) {
      return false;
    }
  }
  const std::string cleanupPath = moveCleanupPathFor(identity.destinationCachePath);
  if (cleanupPath.empty() || Storage.exists(cleanupPath.c_str()) ||
      !Storage.rename(stagingPath.c_str(), cleanupPath.c_str())) {
    return false;
  }
  // The atomic rename is the commit point: no future Reader open can mistake
  // cleanup leftovers for unpublished state. Recursive deletion is best-effort.
  if (!Storage.removeDir(cleanupPath.c_str())) {
    LOG_ERR("BookCache", "Published state but could not remove cleanup copy: %s", cleanupPath.c_str());
  }
  return true;
}

bool cleanupMergedMoveCopy(const std::string& cleanupPath, const std::string& destinationCachePath,
                           const std::string& destinationBookPath) {
  if (!Storage.exists(cleanupPath.c_str())) return true;
  const std::string markerPath = childPath(cleanupPath, MOVE_READY_MARKER);
  if (Storage.exists(markerPath.c_str())) {
    MoveIdentity identity;
    if (!readMoveIdentity(cleanupPath, identity) || identity.destinationCachePath != destinationCachePath ||
        identity.destinationBookPath != destinationBookPath) {
      return false;
    }
  }
  std::vector<CacheEntry> entries;
  if (!readDirectory(cleanupPath, entries)) return false;
  for (const CacheEntry& entry : entries) {
    if (entry.directory || !isMoveStagingFileName(entry.name)) return false;
    if (entry.name == MOVE_READY_MARKER) continue;
    const std::string destination = entry.name == MOVE_BOOKMARK_PAYLOAD
                                        ? BookmarkUtil::getBookmarkPath(destinationBookPath)
                                        : childPath(destinationCachePath, entry.name);
    if (!Storage.exists(destination.c_str()) || !filesEqual(childPath(cleanupPath, entry.name), destination)) {
      return false;
    }
  }
  return Storage.removeDir(cleanupPath.c_str());
}

bool hasOnlyClearStagingEntries(const std::string& path) {
  std::vector<CacheEntry> entries;
  if (!readDirectory(path, entries)) return false;
  return std::all_of(entries.begin(), entries.end(),
                     [](const CacheEntry& entry) { return !entry.directory && isUserStateFileName(entry.name); });
}

bool readOwnedMoveDirectory(const std::string& path, const std::string& destinationCachePath,
                            const std::string& destinationBookPath, MoveIdentity& identity) {
  if (!readMoveIdentity(path, identity) || identity.destinationCachePath != destinationCachePath ||
      identity.destinationBookPath != destinationBookPath) {
    return false;
  }
  std::vector<CacheEntry> entries;
  if (!readDirectory(path, entries)) return false;
  return std::all_of(entries.begin(), entries.end(),
                     [](const CacheEntry& entry) { return !entry.directory && isMoveStagingFileName(entry.name); });
}

std::string replacementDiscardPathFor(const std::string& path, const unsigned attempt) {
  const size_t separator = path.rfind('/');
  const size_t nameOffset = separator == std::string::npos ? 0 : separator + 1;
  if (nameOffset >= path.size()) return {};
  std::string result = path.substr(0, nameOffset) + REPLACEMENT_DISCARD_PREFIX + path.substr(nameOffset);
  if (attempt > 0) result += "_" + std::to_string(attempt + 1);
  return result;
}

bool quarantineReplacementDirectory(const std::string& path) {
  if (!Storage.exists(path.c_str())) return true;
  if (!isDirectory(path)) return false;

  constexpr unsigned MAX_DISCARD_SLOTS = 4;
  for (unsigned attempt = 0; attempt < MAX_DISCARD_SLOTS; ++attempt) {
    const std::string discardPath = replacementDiscardPathFor(path, attempt);
    if (discardPath.empty()) return false;
    // A name alone does not prove that a pre-existing discard directory was
    // created by us or still contains only non-authoritative bytes. Never
    // reclaim it speculatively; use a fresh bounded slot instead.
    if (Storage.exists(discardPath.c_str())) continue;
    if (!Storage.rename(path.c_str(), discardPath.c_str())) return false;
    // The rename is the safety boundary: recovery no longer recognises these
    // bytes. Recursive deletion may be retried by a later reset.
    if (!Storage.removeDir(discardPath.c_str())) {
      LOG_ERR("BookCache", "Replacement state quarantined but not deleted: %s", discardPath.c_str());
    }
    return true;
  }
  return false;
}

bool discardOrphanedMoveSource(const MoveIdentity& identity) {
  if (Storage.exists(identity.sourceBookPath.c_str())) return true;
  bool discarded = true;
  if (Storage.exists(identity.sourceCachePath.c_str())) {
    discarded = quarantineReplacementDirectory(identity.sourceCachePath) && discarded;
  }
  // The old source path no longer owns any bookmarks. Keep an ambiguous
  // legacy file for compatibility, but hide it behind verified empty state.
  discarded = BookmarkUtil::writeEmptyCanonicalBookmark(identity.sourceBookPath) && discarded;
  return discarded;
}

bool discardMovesWhoseSourceWasReplaced(const std::string& bookPath) {
  HalFile root = Storage.open("/.crosspoint");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return true;
  }

  struct PendingDiscard {
    std::string path;
    MoveIdentity identity;
  };
  std::vector<PendingDiscard> pending;
  char name[256]{};
  for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    const bool directory = entry.isDirectory();
    const size_t length = entry.getName(name, sizeof(name));
    entry.close();
    if (!directory || length == 0 || length >= sizeof(name)) continue;

    const std::string entryName(name, length);
    const bool moveStaging = entryName.compare(0, sizeof(MOVE_STAGING_PREFIX) - 1, MOVE_STAGING_PREFIX) == 0;
    const bool moveCleanup = entryName.compare(0, sizeof(MOVE_CLEANUP_PREFIX) - 1, MOVE_CLEANUP_PREFIX) == 0;
    const bool cache = isBookCacheDirectoryName(entryName.c_str());
    if (!moveStaging && !moveCleanup && !cache) continue;

    const std::string path = std::string("/.crosspoint/") + entryName;
    MoveIdentity identity;
    if (!readMoveIdentity(path, identity) || identity.sourceBookPath != bookPath ||
        Storage.exists(identity.destinationBookPath.c_str())) {
      continue;
    }
    if ((moveStaging && moveStagingPathFor(identity.destinationCachePath) != path) ||
        (moveCleanup && moveCleanupPathFor(identity.destinationCachePath) != path)) {
      continue;
    }
    if (moveStaging || moveCleanup) {
      std::vector<CacheEntry> children;
      if (!readDirectory(path, children) || !std::all_of(children.begin(), children.end(), [](const CacheEntry& child) {
            return !child.directory && isMoveStagingFileName(child.name);
          })) {
        continue;
      }
    } else if (identity.destinationCachePath != path) {
      continue;
    }
    pending.push_back({path, std::move(identity)});
  }
  root.close();

  bool discarded = true;
  for (const PendingDiscard& item : pending) {
    discarded = quarantineReplacementDirectory(item.path) && discarded;
    discarded = BookmarkUtil::writeEmptyCanonicalBookmark(item.identity.destinationBookPath) && discarded;
  }
  return discarded;
}

bool publishPreparedMove(const std::string& stagingPath, const MoveIdentity& identity) {
  if (!moveBoundaryCrossed(identity) || !validateMoveStaging(stagingPath, identity) ||
      !publishStagedBookmark(stagingPath, identity)) {
    return false;
  }
  if (Storage.exists(identity.destinationCachePath.c_str())) {
    return mergePreparedMoveIntoDerivedCache(stagingPath, identity);
  }
  if (!Storage.rename(stagingPath.c_str(), identity.destinationCachePath.c_str())) return false;
  return true;
}

bool restoreStagedUserState(const std::string& cachePath, const std::string& stagingPath) {
  std::vector<CacheEntry> entries;
  if (!readDirectory(stagingPath, entries)) {
    LOG_ERR("BookCache", "Could not read cache-clear staging directory: %s", stagingPath.c_str());
    return false;
  }
  for (const CacheEntry& entry : entries) {
    if (entry.directory || !isUserStateFileName(entry.name)) {
      LOG_ERR("BookCache", "Unexpected cache-clear staging entry; leaving staging untouched: %s", entry.name.c_str());
      return false;
    }
  }

  if (!entries.empty()) {
    if (Storage.exists(cachePath.c_str())) {
      if (!isDirectory(cachePath)) return false;
    } else if (!Storage.mkdir(cachePath.c_str())) {
      LOG_ERR("BookCache", "Could not recreate cache directory for state recovery: %s", cachePath.c_str());
      return false;
    }
  }

  bool restored = true;
  for (const CacheEntry& entry : entries) {
    const std::string source = childPath(stagingPath, entry.name);
    const std::string destination = childPath(cachePath, entry.name);
    if (Storage.exists(destination.c_str())) {
      LOG_ERR("BookCache", "Cache state recovery conflict; preserving both copies: %s", destination.c_str());
      restored = false;
      continue;
    }
    if (!Storage.rename(source.c_str(), destination.c_str())) {
      LOG_ERR("BookCache", "Could not restore staged cache state: %s", destination.c_str());
      restored = false;
    }
  }
  if (!restored) return false;
  if (!Storage.rmdir(stagingPath.c_str())) {
    LOG_ERR("BookCache", "Could not remove empty cache-clear staging directory: %s", stagingPath.c_str());
    return false;
  }
  return true;
}

bool rollbackStagedUserState(const std::string& cachePath, const std::string& stagingPath,
                             const std::vector<std::string>& stagedNames) {
  bool restored = true;
  for (auto name = stagedNames.rbegin(); name != stagedNames.rend(); ++name) {
    const std::string source = childPath(stagingPath, *name);
    const std::string destination = childPath(cachePath, *name);
    if (Storage.exists(destination.c_str()) || !Storage.rename(source.c_str(), destination.c_str())) {
      restored = false;
    }
  }
  return restored && Storage.rmdir(stagingPath.c_str());
}

bool clearDerivedCacheEntries(const std::string& cachePath) {
  std::vector<CacheEntry> entries;
  if (!readDirectory(cachePath, entries)) return false;

  for (const CacheEntry& entry : entries) {
    if (!entry.directory && isUserStateFileName(entry.name)) {
      LOG_ERR("BookCache", "User state appeared while clearing cache; aborting: %s", entry.name.c_str());
      return false;
    }
    const std::string path = childPath(cachePath, entry.name);
    const bool removed = entry.directory ? Storage.removeDir(path.c_str()) : Storage.remove(path.c_str());
    if (!removed) {
      LOG_ERR("BookCache", "Could not remove derived cache entry: %s", path.c_str());
      return false;
    }
  }
  return true;
}

}  // namespace

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";

  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0;
}

void clearBookCache(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    Epub(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasTxtExtension(path)) {
    Txt(path, "/.crosspoint").clearCache();
  } else {
    return;
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}

bool clearBookCacheDirectoryPreservingUserState(const std::string& rawCachePath) {
  const std::string cachePath = normalizedCachePath(rawCachePath);
  const std::string stagingPath = stagingPathFor(cachePath);
  if (cachePath.empty() || cachePath == "/" || stagingPath.empty()) return false;

  // A previous attempt may have stopped at any point. Staging only ever holds
  // whitelisted user state, so restore it before retrying the clear.
  if (Storage.exists(stagingPath.c_str()) && !restoreStagedUserState(cachePath, stagingPath)) return false;
  if (!Storage.exists(cachePath.c_str())) return true;
  if (!isDirectory(cachePath)) return false;

  std::vector<CacheEntry> entries;
  if (!readDirectory(cachePath, entries)) return false;

  std::vector<std::string> userStateNames;
  for (const CacheEntry& entry : entries) {
    if (!entry.directory && isUserStateFileName(entry.name)) userStateNames.push_back(entry.name);
  }

  if (userStateNames.empty()) return clearDerivedCacheEntries(cachePath);
  if (!Storage.mkdir(stagingPath.c_str())) {
    LOG_ERR("BookCache", "Could not create cache-clear staging directory: %s", stagingPath.c_str());
    return false;
  }

  std::vector<std::string> stagedNames;
  stagedNames.reserve(userStateNames.size());
  for (const std::string& name : userStateNames) {
    const std::string source = childPath(cachePath, name);
    const std::string destination = childPath(stagingPath, name);
    if (!Storage.rename(source.c_str(), destination.c_str())) {
      LOG_ERR("BookCache", "Could not stage cache state: %s", source.c_str());
      if (!rollbackStagedUserState(cachePath, stagingPath, stagedNames)) {
        LOG_ERR("BookCache", "Cache state rollback incomplete; staging retained for recovery: %s", stagingPath.c_str());
      }
      return false;
    }
    stagedNames.push_back(name);
  }

  if (!clearDerivedCacheEntries(cachePath)) {
    if (!rollbackStagedUserState(cachePath, stagingPath, stagedNames)) {
      LOG_ERR("BookCache", "Cache state rollback incomplete; staging retained for recovery: %s", stagingPath.c_str());
    }
    return false;
  }

  return restoreStagedUserState(cachePath, stagingPath);
}

bool recoverBookCacheUserState(const std::string& rawCachePath, const std::string& bookPath) {
  const std::string cachePath = normalizedCachePath(rawCachePath);
  const std::string moveStagingPath = moveStagingPathFor(cachePath);
  const std::string moveCleanupPath = moveCleanupPathFor(cachePath);
  const std::string stagingPath = stagingPathFor(cachePath);
  if (cachePath.empty() || cachePath == "/" || bookPath.empty() || bookPath == "/" || moveStagingPath.empty() ||
      moveCleanupPath.empty() || stagingPath.empty()) {
    return false;
  }
  if (Storage.exists(moveStagingPath.c_str())) {
    MoveIdentity identity;
    if (!readMoveIdentity(moveStagingPath, identity) || identity.destinationCachePath != cachePath ||
        identity.destinationBookPath != bookPath || !publishPreparedMove(moveStagingPath, identity)) {
      LOG_ERR("BookCache", "Could not recover prepared book-state move: %s", moveStagingPath.c_str());
      return false;
    }
    if (!completeBookCacheUserStateMove(identity.sourceCachePath, identity.destinationCachePath,
                                        identity.sourceBookPath, identity.destinationBookPath)) {
      LOG_ERR("BookCache", "Recovered book state but could not remove the old bookmark copy");
    }
  } else if (Storage.exists(childPath(cachePath, MOVE_READY_MARKER).c_str())) {
    MoveIdentity identity;
    if (!readMoveIdentity(cachePath, identity) || identity.destinationCachePath != cachePath ||
        identity.destinationBookPath != bookPath) {
      LOG_ERR("BookCache", "Destination cache contains an unrecognized move marker: %s", cachePath.c_str());
      return false;
    }
    if (!completeBookCacheUserStateMove(identity.sourceCachePath, identity.destinationCachePath,
                                        identity.sourceBookPath, identity.destinationBookPath)) {
      LOG_ERR("BookCache", "Could not finish committed book-state cleanup: %s", cachePath.c_str());
      return false;
    }
  }
  if (Storage.exists(moveCleanupPath.c_str()) && !cleanupMergedMoveCopy(moveCleanupPath, cachePath, bookPath)) {
    // Cleanup data is never authoritative after the atomic handoff. Preserve
    // unexpected bytes for diagnosis, but do not block the verified cache.
    LOG_ERR("BookCache", "Could not remove completed move cleanup copy: %s", moveCleanupPath.c_str());
  }
  return !Storage.exists(stagingPath.c_str()) || restoreStagedUserState(cachePath, stagingPath);
}

bool resetBookCacheUserStateAfterReplacement(const std::string& rawCachePath, const std::string& bookPath) {
  const std::string cachePath = normalizedCachePath(rawCachePath);
  const std::string clearStagingPath = stagingPathFor(cachePath);
  const std::string moveStagingPath = moveStagingPathFor(cachePath);
  const std::string moveCleanupPath = moveCleanupPathFor(cachePath);
  if (cachePath.empty() || cachePath == "/" || bookPath.empty() || bookPath == "/" || clearStagingPath.empty() ||
      moveStagingPath.empty() || moveCleanupPath.empty()) {
    return false;
  }

  std::vector<MoveIdentity> ownedMoves;
  bool fatalConflict = false;
  bool cleanupOk = true;

  if (Storage.exists(clearStagingPath.c_str()) && !hasOnlyClearStagingEntries(clearStagingPath)) {
    LOG_ERR("BookCache", "Replacement found unexpected cache-clear staging: %s", clearStagingPath.c_str());
    fatalConflict = true;
  }

  if (Storage.exists(moveStagingPath.c_str())) {
    MoveIdentity identity;
    if (!readOwnedMoveDirectory(moveStagingPath, cachePath, bookPath, identity)) {
      LOG_ERR("BookCache", "Replacement found mismatched move staging: %s", moveStagingPath.c_str());
      fatalConflict = true;
    } else {
      ownedMoves.push_back(std::move(identity));
    }
  }

  if (Storage.exists(cachePath.c_str())) {
    if (!isDirectory(cachePath)) {
      fatalConflict = true;
    } else if (Storage.exists(childPath(cachePath, MOVE_READY_MARKER).c_str())) {
      MoveIdentity identity;
      if (!readMoveIdentity(cachePath, identity) || identity.destinationCachePath != cachePath ||
          identity.destinationBookPath != bookPath) {
        LOG_ERR("BookCache", "Replacement found mismatched committed move marker: %s", cachePath.c_str());
        fatalConflict = true;
      } else {
        ownedMoves.push_back(std::move(identity));
      }
    }
  }

  if (Storage.exists(moveCleanupPath.c_str())) {
    // A marker-less partial cleanup can still be deleted when every remaining
    // byte exactly matches the destination. Otherwise require its full durable
    // identity before treating it as owned by this replacement.
    if (!cleanupMergedMoveCopy(moveCleanupPath, cachePath, bookPath) && Storage.exists(moveCleanupPath.c_str())) {
      MoveIdentity identity;
      if (readOwnedMoveDirectory(moveCleanupPath, cachePath, bookPath, identity)) {
        ownedMoves.push_back(std::move(identity));
      } else {
        LOG_ERR("BookCache", "Replacement preserved unverified move cleanup: %s", moveCleanupPath.c_str());
        cleanupOk = false;
      }
    }
  }

  // Unknown recognised recovery data must remain visible so the Reader fails
  // closed. Do not clear the canonical cache and accidentally hide evidence of
  // an ownership conflict.
  if (fatalConflict) return false;

  bool discarded = true;
  if (Storage.exists(moveStagingPath.c_str())) {
    discarded = quarantineReplacementDirectory(moveStagingPath) && discarded;
  }
  if (Storage.exists(clearStagingPath.c_str())) {
    discarded = quarantineReplacementDirectory(clearStagingPath) && discarded;
  }
  if (Storage.exists(moveCleanupPath.c_str()) && cleanupOk) {
    discarded = quarantineReplacementDirectory(moveCleanupPath) && discarded;
  }
  if (Storage.exists(cachePath.c_str())) {
    discarded = quarantineReplacementDirectory(cachePath) && discarded;
  }
  if (!discarded) return false;

  for (const MoveIdentity& identity : ownedMoves) {
    discarded = discardOrphanedMoveSource(identity) && discarded;
  }
  // A reset before the physical rename leaves its transaction under the
  // destination key. Scan only exact, valid identities whose destination book
  // is still absent; malformed or ambiguous entries remain untouched.
  discarded = discardMovesWhoseSourceWasReplaced(bookPath) && discarded;
  return discarded && cleanupOk;
}

bool prepareBookCacheUserStateMove(const std::string& rawSourceCachePath, const std::string& rawDestinationCachePath,
                                   const std::string& sourceBookPath, const std::string& destinationBookPath) {
  const std::string sourceCachePath = normalizedCachePath(rawSourceCachePath);
  const std::string destinationCachePath = normalizedCachePath(rawDestinationCachePath);
  const std::string stagingPath = moveStagingPathFor(destinationCachePath);
  const MoveIdentity identity{sourceBookPath, destinationBookPath, sourceCachePath, destinationCachePath};
  if (!validMoveIdentity(identity) || stagingPath.empty() || Storage.exists(destinationCachePath.c_str()) ||
      !Storage.exists(sourceBookPath.c_str()) || Storage.exists(destinationBookPath.c_str())) {
    return false;
  }

  if (Storage.exists(sourceCachePath.c_str()) && !isDirectory(sourceCachePath)) return false;
  if (Storage.exists(stagingPath.c_str())) {
    if (validateMoveStaging(stagingPath, identity)) {
      const std::string stagedBookmark = childPath(stagingPath, MOVE_BOOKMARK_PAYLOAD);
      const std::string destinationBookmark = BookmarkUtil::getBookmarkPath(destinationBookPath);
      return !Storage.exists(destinationBookmark.c_str()) || BookmarkUtil::isEmptyBookmarkFile(destinationBookmark) ||
             (Storage.exists(stagedBookmark.c_str()) && filesEqual(stagedBookmark, destinationBookmark));
    }
    // A reset before the durable identity marker is written may leave a
    // partial, marker-less copy. Remove it only when every byte is a verified
    // subset of the still-authoritative source; preserve all other content.
    if ((!isNonAuthoritativePreparedMove(stagingPath, identity) &&
         !canDiscardInterruptedMoveStaging(stagingPath, identity)) ||
        !Storage.removeDir(stagingPath.c_str())) {
      return false;
    }
  }
  const std::string destinationBookmark = BookmarkUtil::getBookmarkPath(destinationBookPath);
  if (Storage.exists(destinationBookmark.c_str()) && !BookmarkUtil::isEmptyBookmarkFile(destinationBookmark)) {
    return false;
  }
  if (!Storage.mkdir(stagingPath.c_str())) return false;

  std::vector<CacheEntry> entries;
  bool prepared = !Storage.exists(sourceCachePath.c_str()) || readDirectory(sourceCachePath, entries);
  for (const CacheEntry& entry : entries) {
    if (!prepared) break;
    if (entry.directory || !isMovableUserStateFileName(entry.name)) continue;
    prepared = copyFileExact(childPath(sourceCachePath, entry.name), childPath(stagingPath, entry.name));
  }
  const std::string sourceBookmarkPath = sourceBookmarkPathFor(sourceBookPath);
  if (prepared && Storage.exists(sourceBookmarkPath.c_str())) {
    prepared = copyFileExact(sourceBookmarkPath, childPath(stagingPath, MOVE_BOOKMARK_PAYLOAD));
  } else if (prepared && destinationNeedsEmptyBookmarkPayload(identity)) {
    prepared = BookmarkUtil::writeEmptyBookmarkFile(childPath(stagingPath, MOVE_BOOKMARK_PAYLOAD));
  }
  prepared = prepared && writeMoveReadyMarker(stagingPath, identity) && validateMoveStaging(stagingPath, identity);
  if (!prepared && !Storage.removeDir(stagingPath.c_str())) {
    LOG_ERR("BookCache", "Could not remove incomplete book-state move staging: %s", stagingPath.c_str());
  }
  return prepared;
}

bool finalizeBookCacheUserStateMove(const std::string& rawSourceCachePath, const std::string& rawDestinationCachePath,
                                    const std::string& sourceBookPath, const std::string& destinationBookPath) {
  const std::string sourceCachePath = normalizedCachePath(rawSourceCachePath);
  const std::string destinationCachePath = normalizedCachePath(rawDestinationCachePath);
  const std::string stagingPath = moveStagingPathFor(destinationCachePath);
  const MoveIdentity identity{sourceBookPath, destinationBookPath, sourceCachePath, destinationCachePath};
  return validMoveIdentity(identity) && !stagingPath.empty() && Storage.exists(stagingPath.c_str()) &&
         publishPreparedMove(stagingPath, identity);
}

bool cancelBookCacheUserStateMove(const std::string& rawSourceCachePath, const std::string& rawDestinationCachePath,
                                  const std::string& sourceBookPath, const std::string& destinationBookPath) {
  const std::string sourceCachePath = normalizedCachePath(rawSourceCachePath);
  const std::string destinationCachePath = normalizedCachePath(rawDestinationCachePath);
  const std::string stagingPath = moveStagingPathFor(destinationCachePath);
  const MoveIdentity identity{sourceBookPath, destinationBookPath, sourceCachePath, destinationCachePath};
  if (!validMoveIdentity(identity) || stagingPath.empty()) return false;
  if (!Storage.exists(stagingPath.c_str())) return true;
  if (!validateMoveStaging(stagingPath, identity) && !canDiscardInterruptedMoveStaging(stagingPath, identity)) {
    return false;
  }
  return removePreparedBookmarkCopy(stagingPath, identity) && Storage.removeDir(stagingPath.c_str());
}

bool discardPublishedBookCacheUserStateMove(const std::string& rawSourceCachePath,
                                            const std::string& rawDestinationCachePath,
                                            const std::string& sourceBookPath, const std::string& destinationBookPath) {
  const std::string sourceCachePath = normalizedCachePath(rawSourceCachePath);
  const std::string destinationCachePath = normalizedCachePath(rawDestinationCachePath);
  const MoveIdentity identity{sourceBookPath, destinationBookPath, sourceCachePath, destinationCachePath};
  if (!validMoveIdentity(identity) || !Storage.exists(sourceBookPath.c_str()) ||
      Storage.exists(destinationBookPath.c_str()) || !isDirectory(sourceCachePath) ||
      !isDirectory(destinationCachePath)) {
    return false;
  }

  std::vector<CacheEntry> destinationEntries;
  if (!readDirectory(destinationCachePath, destinationEntries)) return false;
  std::vector<std::string> destinationNames;
  for (const CacheEntry& entry : destinationEntries) {
    if (entry.directory || (entry.name != MOVE_BOOKMARK_PAYLOAD && entry.name != MOVE_READY_MARKER &&
                            !isMovableUserStateFileName(entry.name))) {
      return false;
    }
    if (entry.name == MOVE_BOOKMARK_PAYLOAD || entry.name == MOVE_READY_MARKER) continue;
    destinationNames.push_back(entry.name);
  }

  std::vector<CacheEntry> sourceEntries;
  if (!readDirectory(sourceCachePath, sourceEntries)) return false;
  std::vector<std::string> sourceNames;
  for (const CacheEntry& entry : sourceEntries) {
    if (!entry.directory && isMovableUserStateFileName(entry.name)) sourceNames.push_back(entry.name);
  }
  std::sort(sourceNames.begin(), sourceNames.end());
  std::sort(destinationNames.begin(), destinationNames.end());
  if (sourceNames != destinationNames) return false;
  if (!std::all_of(sourceNames.begin(), sourceNames.end(), [&](const std::string& name) {
        return filesEqual(childPath(sourceCachePath, name), childPath(destinationCachePath, name));
      })) {
    return false;
  }
  const std::string sourceBookmark = sourceBookmarkPathFor(sourceBookPath);
  const std::string destinationBookmark = BookmarkUtil::getBookmarkPath(destinationBookPath);
  if (Storage.exists(destinationBookmark.c_str())) {
    if (BookmarkUtil::isEmptyBookmarkFile(destinationBookmark)) {
      // Empty tombstones carry no user data and are safe to retain on rollback.
    } else if (!Storage.exists(sourceBookmark.c_str()) || !filesEqual(sourceBookmark, destinationBookmark) ||
               !Storage.remove(destinationBookmark.c_str())) {
      return false;
    }
  }
  return Storage.removeDir(destinationCachePath.c_str());
}

bool completeBookCacheUserStateMove(const std::string& rawSourceCachePath, const std::string& rawDestinationCachePath,
                                    const std::string& sourceBookPath, const std::string& destinationBookPath) {
  const MoveIdentity identity{sourceBookPath, destinationBookPath, normalizedCachePath(rawSourceCachePath),
                              normalizedCachePath(rawDestinationCachePath)};
  if (!validMoveIdentity(identity) || !moveBoundaryCrossed(identity) ||
      !Storage.exists(identity.destinationCachePath.c_str())) {
    return false;
  }
  MoveIdentity storedIdentity;
  if (!readMoveIdentity(identity.destinationCachePath, storedIdentity) || !(storedIdentity == identity)) return false;

  const std::string sourceBookmark = BookmarkUtil::getBookmarkPath(sourceBookPath);
  const std::string destinationBookmark = BookmarkUtil::getBookmarkPath(destinationBookPath);
  if (Storage.exists(sourceBookmark.c_str()) &&
      (!Storage.exists(destinationBookmark.c_str()) || !filesEqual(sourceBookmark, destinationBookmark) ||
       !Storage.remove(sourceBookmark.c_str()))) {
    return false;
  }
  if (!BookmarkUtil::ensureLegacyBookmarkShadowed(sourceBookPath)) return false;
  if (Storage.exists(identity.sourceCachePath.c_str()) && !Storage.removeDir(identity.sourceCachePath.c_str())) {
    return false;
  }

  const std::string payload = childPath(identity.destinationCachePath, MOVE_BOOKMARK_PAYLOAD);
  if (Storage.exists(payload.c_str()) && !Storage.remove(payload.c_str())) return false;
  return Storage.remove(childPath(identity.destinationCachePath, MOVE_READY_MARKER).c_str());
}
