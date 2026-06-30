#include "TocBoundaryCache.h"

#include <HalStorage.h>

#include "../../Serialization/Serialization.h"
#include "Epub.h"
#include "Section.h"

TocBoundaryCache::TocBoundaryCache(std::shared_ptr<Epub> epub, int spineIndex,
                                   GetPageForAnchorFn getPageForAnchorCallback)
    : epub(epub), spineIndex(spineIndex), getPageForAnchorCallback(std::move(getPageForAnchorCallback)) {}

// Lazily computes the list of TOC entries for the current spine.
// This batches the getTocItem file reads to be done all together and caches
// them so file reads from the cache aren't needed.
//
// Actual metadata computation (like start page) is lazily deferred until
// requested per TOC entry.
//
void TocBoundaryCache::ensureEntriesLoaded() {
  if (!baseTocIndex) {
    baseTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  }

  if (*baseTocIndex == -1) {
    return;
  }

  if (!entries) {
    // Find all TOC entries that belong to this spine.
    // For better performance, we assume TOC entries belonging to the same
    // spine are contiguous in the TOC list and are strictly in reading (page)
    // order. This is not guaranteed to be true in EPUBs, but assuming this
    // avoids scanning the entire TOC.
    std::vector<TocEntry> newEntries;

    for (int i = *baseTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex > spineIndex) break;

      std::optional<int32_t> initialStartPage = std::nullopt;
      // Pre-resolving empty anchors avoids future file reads for 1:1 mapped
      // spines, which most well-formed books are.
      if (entry.anchor.empty()) {
        initialStartPage = 0;
      }

      newEntries.push_back({i, initialStartPage, std::nullopt});
    }

    entries = newEntries;
  }
}

std::optional<uint16_t> TocBoundaryCache::getTocStartPage(int tocIndex) {
  if (auto entry = getEntry(tocIndex)) {
    if (!entry->startPageInSpine) {
      auto tocItem = epub->getTocItem(tocIndex);
      if (auto p = getPageForAnchorCallback(tocItem.anchor)) {
        entry->startPageInSpine = *p;
      } else {
        entry->startPageInSpine = -1;
      }
    }
    return entry->startPageInSpine.value() >= 0 ? std::optional<uint16_t>(entry->startPageInSpine.value())
                                                : std::nullopt;
  }

  return std::nullopt;
}

// Finds the closest TOC entry for the given page.
// Assumes TOC entries are strictly in reading (page) order.
int TocBoundaryCache::getTocIndexForPage(const int pageInSpine) {
  ensureEntriesLoaded();

  if (!baseTocIndex || *baseTocIndex == -1) {
    return -1;
  }

  int bestTocIndex = *baseTocIndex;

  if (entries && !entries->empty()) {
    if (auto firstPage = getTocStartPage(entries->front().tocIndex)) {
      if (pageInSpine < firstPage.value()) {
        return bestTocIndex > 0 ? bestTocIndex - 1 : -1;
      }
    }

    for (auto& entry : *entries) {
      if (auto p = getTocStartPage(entry.tocIndex)) {
        if (p.value() <= pageInSpine) {
          bestTocIndex = entry.tocIndex;
        } else {
          break;
        }
      }
    }
  }

  return bestTocIndex;
}

TocBoundaryCache::TocEntry* TocBoundaryCache::getEntry(int tocIndex) {
  ensureEntriesLoaded();
  if (entries) {
    for (auto& entry : *entries) {
      if (entry.tocIndex == tocIndex) {
        return &entry;
      }
    }
  }
  return nullptr;
}
