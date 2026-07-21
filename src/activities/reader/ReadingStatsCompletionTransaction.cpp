#include "ReadingStatsCompletionTransaction.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "ReadingStatsCodec.h"
#include "ReadingStatsStorage.h"

namespace {
constexpr char LOG_TAG[] = "STATSTXN";
constexpr char MARKER_PATH[] = "/.crosspoint/stats_completion.txn";
constexpr char MARKER_TEMP_PATH[] = "/.crosspoint/stats_completion.txn.tmp";
constexpr std::array<uint8_t, 4> MARKER_MAGIC = {'C', 'V', 'S', 'T'};
constexpr uint8_t MARKER_VERSION = 1;
constexpr uint8_t FLAG_BOOK_WAS_MISSING = 1u << 0;
constexpr uint8_t FLAG_GLOBAL_WAS_MISSING = 1u << 1;
constexpr uint8_t KNOWN_FLAGS = FLAG_BOOK_WAS_MISSING | FLAG_GLOBAL_WAS_MISSING;
constexpr char EPUB_CACHE_PREFIX[] = "/.crosspoint/epub_";
constexpr size_t MAX_CACHE_PATH_SIZE = 64;
constexpr size_t HEADER_SIZE = 8;
constexpr size_t PAYLOAD_SIZE = BookReadingStats::CURRENT_FILE_SIZE * 2 + GlobalReadingStats::CURRENT_FILE_SIZE * 2;
constexpr size_t CRC_SIZE = 4;
constexpr size_t MAX_MARKER_SIZE = HEADER_SIZE + MAX_CACHE_PATH_SIZE + PAYLOAD_SIZE + CRC_SIZE;

using BookBytes = ReadingStatsCodec::BookBytes;
using GlobalBytes = ReadingStatsCodec::GlobalBytes;

enum class MarkerReadResult : uint8_t { Missing, Ok, Blocked };

struct Marker {
  std::string cachePath;
  BookReadingStats oldBookStats;
  BookReadingStats newBookStats;
  GlobalReadingStats oldGlobalStats;
  GlobalReadingStats newGlobalStats;
  bool bookWasMissing = false;
  bool globalWasMissing = false;
  std::vector<uint8_t> encoded;
};

uint16_t readLe16(const uint8_t* data, const size_t offset) {
  return static_cast<uint16_t>(data[offset]) | static_cast<uint16_t>(data[offset + 1]) << 8;
}

uint32_t readLe32(const uint8_t* data, const size_t offset) {
  return static_cast<uint32_t>(data[offset]) | static_cast<uint32_t>(data[offset + 1]) << 8 |
         static_cast<uint32_t>(data[offset + 2]) << 16 | static_cast<uint32_t>(data[offset + 3]) << 24;
}

void writeLe16(uint8_t* data, const size_t offset, const uint16_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void writeLe32(uint8_t* data, const size_t offset, const uint32_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
  data[offset + 2] = static_cast<uint8_t>(value >> 16);
  data[offset + 3] = static_cast<uint8_t>(value >> 24);
}

uint32_t crc32(const uint8_t* data, const size_t size) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

bool validCachePath(const std::string& path) {
  constexpr size_t prefixSize = sizeof(EPUB_CACHE_PREFIX) - 1;
  return path.size() > prefixSize && path.size() <= MAX_CACHE_PATH_SIZE &&
         path.compare(0, prefixSize, EPUB_CACHE_PREFIX) == 0 &&
         std::all_of(path.begin() + static_cast<std::string::difference_type>(prefixSize), path.end(),
                     [](const char character) { return character >= '0' && character <= '9'; });
}

bool equalExceptBookCompletionFields(const BookBytes& lhs, const BookBytes& rhs) {
  for (size_t i = 0; i < lhs.size(); ++i) {
    const bool completionField = i == 11 || (i >= 21 && i <= 24) || i >= 69;
    if (!completionField && lhs[i] != rhs[i]) return false;
  }
  return true;
}

bool equalExceptGlobalCompletedBooks(const GlobalBytes& lhs, const GlobalBytes& rhs) {
  for (size_t i = 0; i < lhs.size(); ++i) {
    if ((i < 13 || i > 16) && lhs[i] != rhs[i]) return false;
  }
  return true;
}

bool validTransition(const BookReadingStats& oldBook, const BookReadingStats& newBook,
                     const GlobalReadingStats& oldGlobal, const GlobalReadingStats& newGlobal) {
  if (oldBook.isCompleted == newBook.isCompleted) return false;
  const BookBytes oldBookBytes = ReadingStatsCodec::encode(oldBook);
  const BookBytes newBookBytes = ReadingStatsCodec::encode(newBook);
  const GlobalBytes oldGlobalBytes = ReadingStatsCodec::encode(oldGlobal);
  const GlobalBytes newGlobalBytes = ReadingStatsCodec::encode(newGlobal);
  if (!equalExceptBookCompletionFields(oldBookBytes, newBookBytes) ||
      !equalExceptGlobalCompletedBooks(oldGlobalBytes, newGlobalBytes)) {
    return false;
  }

  const uint32_t expectedCompletedBooks =
      newBook.isCompleted
          ? (oldGlobal.completedBooks == std::numeric_limits<uint32_t>::max() ? oldGlobal.completedBooks
                                                                              : oldGlobal.completedBooks + 1)
          : (oldGlobal.completedBooks == 0 ? 0 : oldGlobal.completedBooks - 1);
  return newGlobal.completedBooks == expectedCompletedBooks &&
         (!newBook.isCompleted || newBook.estimatedTimeLeftSeconds == 0);
}

std::vector<uint8_t> encodeMarker(const std::string& cachePath, const BookReadingStats& oldBook,
                                  const BookReadingStats& newBook, const GlobalReadingStats& oldGlobal,
                                  const GlobalReadingStats& newGlobal, const bool bookWasMissing,
                                  const bool globalWasMissing) {
  const BookBytes oldBookBytes = ReadingStatsCodec::encode(oldBook);
  const BookBytes newBookBytes = ReadingStatsCodec::encode(newBook);
  const GlobalBytes oldGlobalBytes = ReadingStatsCodec::encode(oldGlobal);
  const GlobalBytes newGlobalBytes = ReadingStatsCodec::encode(newGlobal);
  std::vector<uint8_t> bytes(HEADER_SIZE + cachePath.size() + PAYLOAD_SIZE + CRC_SIZE, 0);
  std::copy(MARKER_MAGIC.begin(), MARKER_MAGIC.end(), bytes.begin());
  bytes[4] = MARKER_VERSION;
  bytes[5] = (bookWasMissing ? FLAG_BOOK_WAS_MISSING : 0u) | (globalWasMissing ? FLAG_GLOBAL_WAS_MISSING : 0u);
  writeLe16(bytes.data(), 6, static_cast<uint16_t>(cachePath.size()));
  size_t offset = HEADER_SIZE;
  memcpy(bytes.data() + offset, cachePath.data(), cachePath.size());
  offset += cachePath.size();
  memcpy(bytes.data() + offset, oldBookBytes.data(), oldBookBytes.size());
  offset += oldBookBytes.size();
  memcpy(bytes.data() + offset, newBookBytes.data(), newBookBytes.size());
  offset += newBookBytes.size();
  memcpy(bytes.data() + offset, oldGlobalBytes.data(), oldGlobalBytes.size());
  offset += oldGlobalBytes.size();
  memcpy(bytes.data() + offset, newGlobalBytes.data(), newGlobalBytes.size());
  offset += newGlobalBytes.size();
  writeLe32(bytes.data(), offset, crc32(bytes.data(), offset));
  return bytes;
}

bool decodeMarker(const uint8_t* data, const size_t size, Marker& marker) {
  marker = {};
  if (!data || size < HEADER_SIZE + PAYLOAD_SIZE + CRC_SIZE ||
      !std::equal(MARKER_MAGIC.begin(), MARKER_MAGIC.end(), data) || data[4] != MARKER_VERSION ||
      (data[5] & ~KNOWN_FLAGS) != 0) {
    return false;
  }
  const size_t pathSize = readLe16(data, 6);
  if (pathSize == 0 || pathSize > MAX_CACHE_PATH_SIZE || size != HEADER_SIZE + pathSize + PAYLOAD_SIZE + CRC_SIZE ||
      readLe32(data, size - CRC_SIZE) != crc32(data, size - CRC_SIZE)) {
    return false;
  }

  marker.cachePath.assign(reinterpret_cast<const char*>(data + HEADER_SIZE), pathSize);
  if (!validCachePath(marker.cachePath)) return false;
  size_t offset = HEADER_SIZE + pathSize;
  if (ReadingStatsCodec::decode(data + offset, BookReadingStats::CURRENT_FILE_SIZE, marker.oldBookStats) !=
      ReadingStatsDecodeResult::Ok)
    return false;
  offset += BookReadingStats::CURRENT_FILE_SIZE;
  if (ReadingStatsCodec::decode(data + offset, BookReadingStats::CURRENT_FILE_SIZE, marker.newBookStats) !=
      ReadingStatsDecodeResult::Ok)
    return false;
  offset += BookReadingStats::CURRENT_FILE_SIZE;
  if (ReadingStatsCodec::decode(data + offset, GlobalReadingStats::CURRENT_FILE_SIZE, marker.oldGlobalStats) !=
      ReadingStatsDecodeResult::Ok)
    return false;
  offset += GlobalReadingStats::CURRENT_FILE_SIZE;
  if (ReadingStatsCodec::decode(data + offset, GlobalReadingStats::CURRENT_FILE_SIZE, marker.newGlobalStats) !=
      ReadingStatsDecodeResult::Ok)
    return false;

  marker.bookWasMissing = (data[5] & FLAG_BOOK_WAS_MISSING) != 0;
  marker.globalWasMissing = (data[5] & FLAG_GLOBAL_WAS_MISSING) != 0;
  marker.encoded.assign(data, data + size);
  return validTransition(marker.oldBookStats, marker.newBookStats, marker.oldGlobalStats, marker.newGlobalStats);
}

MarkerReadResult readOneMarker(const char* path, Marker& marker) {
  std::array<uint8_t, MAX_MARKER_SIZE> bytes{};
  const ReadingStatsStorage::ReadOutcome read = ReadingStatsStorage::read(path, bytes.data(), bytes.size());
  if (read.result == ReadingStatsStorage::ReadResult::Missing) return MarkerReadResult::Missing;
  if (read.result != ReadingStatsStorage::ReadResult::Ok || !decodeMarker(bytes.data(), read.size, marker)) {
    return MarkerReadResult::Blocked;
  }
  return MarkerReadResult::Ok;
}

MarkerReadResult readMarker(Marker& marker) {
  Marker primary;
  Marker temporary;
  const MarkerReadResult primaryResult = readOneMarker(MARKER_PATH, primary);
  const MarkerReadResult temporaryResult = readOneMarker(MARKER_TEMP_PATH, temporary);
  if (primaryResult == MarkerReadResult::Blocked || temporaryResult == MarkerReadResult::Blocked) {
    return MarkerReadResult::Blocked;
  }
  if (primaryResult == MarkerReadResult::Missing && temporaryResult == MarkerReadResult::Missing) {
    return MarkerReadResult::Missing;
  }
  if (primaryResult == MarkerReadResult::Ok && temporaryResult == MarkerReadResult::Ok &&
      primary.encoded != temporary.encoded) {
    return MarkerReadResult::Blocked;
  }
  marker = primaryResult == MarkerReadResult::Ok ? std::move(primary) : std::move(temporary);
  return MarkerReadResult::Ok;
}

enum class PayloadState : uint8_t { Old, New, Mismatch };

template <typename Bytes>
PayloadState classifyPayload(const Bytes& current, const Bytes& oldPayload, const Bytes& newPayload,
                             const bool wasMissing, const bool isMissing) {
  if (!isMissing && current == newPayload) return PayloadState::New;
  if (current == oldPayload && isMissing == wasMissing) return PayloadState::Old;
  return PayloadState::Mismatch;
}

bool removeMarkerFiles() {
  const std::array<const char*, 2> paths = {MARKER_PATH, MARKER_TEMP_PATH};
  return std::all_of(paths.begin(), paths.end(),
                     [](const char* path) { return !Storage.exists(path) || Storage.remove(path); });
}

bool markerAllowsBookWrite(const Marker& marker, const std::string& cachePath, const BookReadingStats& stats) {
  return marker.cachePath == cachePath &&
         ReadingStatsCodec::encode(stats) == ReadingStatsCodec::encode(marker.newBookStats);
}

bool markerAllowsGlobalWrite(const Marker& marker, const GlobalReadingStats& stats) {
  return ReadingStatsCodec::encode(stats) == ReadingStatsCodec::encode(marker.newGlobalStats);
}
}  // namespace

namespace ReadingStatsCompletionTransaction {

RecoveryResult recover(const std::string& cachePath) {
  Marker marker;
  const MarkerReadResult markerResult = readMarker(marker);
  if (markerResult == MarkerReadResult::Missing) return RecoveryResult::NoMarker;
  if (markerResult == MarkerReadResult::Blocked || marker.cachePath != cachePath) {
    LOG_ERR(LOG_TAG, "Completion transaction marker is invalid or belongs to another book");
    return RecoveryResult::Blocked;
  }

  BookReadingStats::LoadStatus bookStatus = BookReadingStats::LoadStatus::Invalid;
  GlobalReadingStats::LoadStatus globalStatus = GlobalReadingStats::LoadStatus::Invalid;
  BookReadingStats currentBook = BookReadingStats::load(cachePath, &bookStatus);
  GlobalReadingStats currentGlobal = GlobalReadingStats::load(&globalStatus);
  if (!BookReadingStats::isTrustedLoadStatus(bookStatus) || !GlobalReadingStats::isTrustedLoadStatus(globalStatus)) {
    LOG_ERR(LOG_TAG, "Completion transaction destination is unreadable or newer");
    return RecoveryResult::Blocked;
  }

  const BookBytes oldBookBytes = ReadingStatsCodec::encode(marker.oldBookStats);
  const BookBytes newBookBytes = ReadingStatsCodec::encode(marker.newBookStats);
  const GlobalBytes oldGlobalBytes = ReadingStatsCodec::encode(marker.oldGlobalStats);
  const GlobalBytes newGlobalBytes = ReadingStatsCodec::encode(marker.newGlobalStats);
  const PayloadState bookState =
      classifyPayload(ReadingStatsCodec::encode(currentBook), oldBookBytes, newBookBytes, marker.bookWasMissing,
                      bookStatus == BookReadingStats::LoadStatus::Missing);
  const PayloadState globalState =
      classifyPayload(ReadingStatsCodec::encode(currentGlobal), oldGlobalBytes, newGlobalBytes, marker.globalWasMissing,
                      globalStatus == GlobalReadingStats::LoadStatus::Missing);
  if (bookState == PayloadState::Mismatch || globalState == PayloadState::Mismatch) {
    LOG_ERR(LOG_TAG, "Completion transaction destination no longer matches its old or new payload");
    return RecoveryResult::Blocked;
  }

  const bool bookNeedsPublish = bookState == PayloadState::Old || bookStatus != BookReadingStats::LoadStatus::Ok;
  const bool globalNeedsPublish =
      globalState == PayloadState::Old || globalStatus != GlobalReadingStats::LoadStatus::Ok;
  if (bookNeedsPublish && !marker.newBookStats.save(cachePath)) {
    LOG_ERR(LOG_TAG, "Could not publish per-book completion state");
    return RecoveryResult::Blocked;
  }
  if (globalNeedsPublish && !marker.newGlobalStats.save()) {
    LOG_ERR(LOG_TAG, "Could not publish global completion state");
    return RecoveryResult::Blocked;
  }

  currentBook = BookReadingStats::load(cachePath, &bookStatus);
  currentGlobal = GlobalReadingStats::load(&globalStatus);
  if (bookStatus != BookReadingStats::LoadStatus::Ok || globalStatus != GlobalReadingStats::LoadStatus::Ok ||
      ReadingStatsCodec::encode(currentBook) != newBookBytes ||
      ReadingStatsCodec::encode(currentGlobal) != newGlobalBytes) {
    LOG_ERR(LOG_TAG, "Completion transaction verification failed");
    return RecoveryResult::Blocked;
  }
  if (!removeMarkerFiles()) {
    LOG_ERR(LOG_TAG, "Completion transaction committed but marker cleanup failed");
    return RecoveryResult::Blocked;
  }
  return RecoveryResult::Recovered;
}

RecoveryResult recoverPending() {
  Marker marker;
  const MarkerReadResult result = readMarker(marker);
  if (result == MarkerReadResult::Missing) return RecoveryResult::NoMarker;
  if (result == MarkerReadResult::Blocked) return RecoveryResult::Blocked;
  // decodeMarker() already constrained this to /.crosspoint/epub_<digits>.
  return recover(marker.cachePath);
}

bool commit(const std::string& cachePath, const BookReadingStats& oldBookStats, const BookReadingStats& newBookStats,
            const GlobalReadingStats& oldGlobalStats, const GlobalReadingStats& newGlobalStats) {
  if (!validCachePath(cachePath) || !validTransition(oldBookStats, newBookStats, oldGlobalStats, newGlobalStats)) {
    return false;
  }
  const RecoveryResult pending = recoverPending();
  if (pending == RecoveryResult::Blocked) return false;

  BookReadingStats::LoadStatus bookStatus = BookReadingStats::LoadStatus::Invalid;
  GlobalReadingStats::LoadStatus globalStatus = GlobalReadingStats::LoadStatus::Invalid;
  const BookReadingStats currentBook = BookReadingStats::load(cachePath, &bookStatus);
  const GlobalReadingStats currentGlobal = GlobalReadingStats::load(&globalStatus);
  if (!BookReadingStats::isTrustedLoadStatus(bookStatus) || !GlobalReadingStats::isTrustedLoadStatus(globalStatus) ||
      ReadingStatsCodec::encode(currentBook) != ReadingStatsCodec::encode(oldBookStats) ||
      ReadingStatsCodec::encode(currentGlobal) != ReadingStatsCodec::encode(oldGlobalStats)) {
    return false;
  }

  const std::vector<uint8_t> marker = encodeMarker(cachePath, oldBookStats, newBookStats, oldGlobalStats,
                                                   newGlobalStats, bookStatus == BookReadingStats::LoadStatus::Missing,
                                                   globalStatus == GlobalReadingStats::LoadStatus::Missing);
  if (!ReadingStatsStorage::writeAtomic(MARKER_PATH, nullptr, false, marker.data(), marker.size())) return false;
  return recover(cachePath) == RecoveryResult::Recovered;
}

bool permitsBookWrite(const std::string& cachePath, const BookReadingStats& stats) {
  Marker marker;
  const MarkerReadResult result = readMarker(marker);
  return result == MarkerReadResult::Missing ||
         (result == MarkerReadResult::Ok && markerAllowsBookWrite(marker, cachePath, stats));
}

bool canRelocateOrDeleteEpubCache(const std::string& cachePath) {
  Marker marker;
  const MarkerReadResult result = readMarker(marker);
  return result == MarkerReadResult::Missing || (result == MarkerReadResult::Ok && marker.cachePath != cachePath);
}

bool permitsGlobalWrite(const GlobalReadingStats& stats) {
  Marker marker;
  const MarkerReadResult result = readMarker(marker);
  return result == MarkerReadResult::Missing ||
         (result == MarkerReadResult::Ok && markerAllowsGlobalWrite(marker, stats));
}

}  // namespace ReadingStatsCompletionTransaction
