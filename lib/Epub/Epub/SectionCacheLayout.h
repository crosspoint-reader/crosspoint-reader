#pragma once

#include <cstdint>

namespace SectionCacheLayout {

struct FinalizedHeader {
  uint16_t pageCount = 0;
  uint32_t pageLutOffset = 0;
  uint32_t anchorMapOffset = 0;
  uint32_t paragraphLutOffset = 0;
  uint32_t listItemLutOffset = 0;
};

inline bool hasCompleteHeader(const uint64_t fileSize, const uint64_t headerSize) {
  return headerSize > 0 && fileSize >= headerSize;
}

inline uint64_t partialTrailerOffset(const uint32_t listItemLutOffset, const uint16_t pageCount) {
  return static_cast<uint64_t>(listItemLutOffset) + static_cast<uint64_t>(pageCount) * sizeof(uint16_t);
}

inline bool hasCompletePartialTrailer(const uint64_t fileSize, const uint64_t headerSize,
                                      const uint32_t listItemLutOffset, const uint16_t pageCount) {
  if (!hasCompleteHeader(fileSize, headerSize) || pageCount == 0 || listItemLutOffset < headerSize) return false;
  const uint64_t trailerOffset = partialTrailerOffset(listItemLutOffset, pageCount);
  constexpr uint64_t TRAILER_SIZE = 2 * sizeof(uint32_t);
  return trailerOffset <= fileSize && TRAILER_SIZE <= fileSize - trailerOffset;
}

inline bool isValidFinalized(const uint64_t fileSize, const uint64_t headerSize, const FinalizedHeader& header,
                             const uint16_t storedParagraphCount) {
  if (!hasCompleteHeader(fileSize, headerSize) || header.pageCount == 0 || storedParagraphCount != header.pageCount ||
      header.pageLutOffset < headerSize) {
    return false;
  }

  const uint64_t pageLutBytes = static_cast<uint64_t>(header.pageCount) * sizeof(uint32_t);
  const uint64_t indexBytes = static_cast<uint64_t>(header.pageCount) * sizeof(uint16_t);
  const uint64_t pageLutEnd = static_cast<uint64_t>(header.pageLutOffset) + pageLutBytes;
  const uint64_t minimumAnchorEnd = static_cast<uint64_t>(header.anchorMapOffset) + sizeof(uint16_t);
  const uint64_t paragraphLutEnd = static_cast<uint64_t>(header.paragraphLutOffset) + sizeof(uint16_t) + indexBytes;
  const uint64_t listItemLutEnd = static_cast<uint64_t>(header.listItemLutOffset) + indexBytes;

  return pageLutEnd == header.anchorMapOffset && minimumAnchorEnd <= header.paragraphLutOffset &&
         paragraphLutEnd == header.listItemLutOffset && listItemLutEnd == fileSize;
}

}  // namespace SectionCacheLayout
