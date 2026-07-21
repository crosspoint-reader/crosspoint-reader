#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "clippings/ClippingStore.h"

namespace SavedClippingsModel {

enum class CatalogState : uint8_t {
  Ready,
  Empty,
  Incomplete,
  ReadError,
};

inline bool hasKnownTimestamp(const ClippingStore::CatalogEntry& entry) {
  // Legacy CrossInk timestamps are not trustworthy, and an unset/broken
  // device clock must not become a plausible-looking date in the UI.
  constexpr uint32_t MIN_TIMESTAMP = 1577836800U;  // 2020-01-01 UTC
  constexpr uint32_t MAX_TIMESTAMP = 4102444799U;  // 2099-12-31 UTC
  return entry.format == ClippingCodec::Format::Current && entry.newestTimestamp >= MIN_TIMESTAMP &&
         entry.newestTimestamp <= MAX_TIMESTAMP;
}

inline bool isComplete(const ClippingStore::CatalogLoadResult loadResult, const ClippingStore::Catalog& catalog) {
  return loadResult == ClippingStore::CatalogLoadResult::Loaded &&
         catalog.entries.size() <= ClippingStore::MAX_CATALOG_BOOKS && !catalog.directoryTruncated &&
         !catalog.entryNameTruncated && catalog.skippedBooks == 0;
}

inline CatalogState state(const ClippingStore::CatalogLoadResult loadResult, const ClippingStore::Catalog& catalog) {
  if (loadResult == ClippingStore::CatalogLoadResult::IoError) return CatalogState::ReadError;
  if (loadResult == ClippingStore::CatalogLoadResult::DirectoryMissing) return CatalogState::Empty;
  if (!isComplete(loadResult, catalog)) return CatalogState::Incomplete;
  return catalog.entries.empty() ? CatalogState::Empty : CatalogState::Ready;
}

inline bool canExport(const ClippingStore::CatalogLoadResult loadResult, const ClippingStore::Catalog& catalog) {
  return state(loadResult, catalog) == CatalogState::Ready;
}

inline void sortEntries(std::vector<ClippingStore::CatalogEntry>& entries) {
  std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
    const bool leftKnown = hasKnownTimestamp(left);
    const bool rightKnown = hasKnownTimestamp(right);
    if (leftKnown != rightKnown) return leftKnown;
    if (leftKnown && left.newestTimestamp != right.newestTimestamp) {
      return left.newestTimestamp > right.newestTimestamp;
    }

    // Untitled books remain deterministic but do not hide named books at the
    // top of the unknown-time group.
    if (left.book.title.empty() != right.book.title.empty()) return !left.book.title.empty();
    if (left.book.title != right.book.title) return left.book.title < right.book.title;
    if (left.book.author != right.book.author) return left.book.author < right.book.author;
    return left.book.path < right.book.path;
  });
}

}  // namespace SavedClippingsModel
