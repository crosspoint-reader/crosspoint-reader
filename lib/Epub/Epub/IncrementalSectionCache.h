#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "IncrementalSectionTypes.h"

class Page;

namespace IncrementalSection {

struct PageIndexRecord {
  uint32_t pageOffset = 0;
  uint32_t pageLength = 0;
  uint16_t paragraphIndex = 0;
  uint16_t listItemIndex = 0;
  uint32_t sourceByteOffset = 0;
};

static_assert(sizeof(PageIndexRecord) == PAGE_INDEX_RECORD_SIZE, "Incremental page index record must stay 16 bytes");

struct Meta {
  CacheState state = CacheState::Building;
  uint32_t spineIndex = 0;
  LayoutCacheKey layoutKey{};
  uint32_t sourceUncompressedSize = 0;
  uint32_t finalPageCount = 0;
  uint32_t anchorCount = 0;
  uint32_t generationId = 0;
};

class Cache {
 public:
  explicit Cache(std::string sectionsDir, uint32_t spineIndex);

  bool loadMeta(Meta& out) const;
  bool beginBuild(const LayoutCacheKey& key, uint32_t sourceUncompressedSize, uint32_t generationId);
  bool appendPage(uint32_t pageNumber, const Page& page, uint16_t paragraphIndex, uint16_t listItemIndex,
                  uint32_t sourceByteOffset = 0);
  bool appendAnchor(const std::string& anchor, uint16_t page);
  bool markComplete(uint32_t finalPageCount, uint32_t anchorCount);
  std::unique_ptr<Page> loadPage(uint32_t pageNumber) const;
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t paragraphIndex) const;
  std::optional<uint16_t> getPageForListItemIndex(uint16_t listItemIndex) const;
  std::optional<uint16_t> getParagraphIndexForPage(uint32_t pageNumber) const;
  uint32_t knownPageCount() const;
  bool isComplete() const;
  bool isCompatibleWith(const LayoutCacheKey& key) const;
  bool hasPage(uint32_t pageNumber) const;
  void removeAllFiles() const;

 private:
  std::string sectionsDir_;
  uint32_t spineIndex_;
  Paths paths_;

  bool writeMeta(const Meta& meta) const;
  bool readIndexRecord(uint32_t pageNumber, PageIndexRecord& out) const;
};

}  // namespace IncrementalSection
