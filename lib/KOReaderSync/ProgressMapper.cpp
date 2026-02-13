#include "ProgressMapper.h"

#include <HardwareSerial.h>

#include <cmath>

KOReaderPosition ProgressMapper::toKOReader(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos) {
  KOReaderPosition result;

  // Calculate page progress within current spine item
  float intraSpineProgress = 0.0f;
  if (pos.totalPages > 0) {
    intraSpineProgress = static_cast<float>(pos.pageNumber) / static_cast<float>(pos.totalPages);
  }

  // Calculate overall book progress (0.0-1.0)
  result.percentage = epub->calculateProgress(pos.spineIndex, intraSpineProgress);

  // Generate XPath with estimated paragraph position based on page
  result.xpath = generateXPath(pos.spineIndex, pos.pageNumber, pos.totalPages);

  // Get chapter info for logging
  const int tocIndex = epub->getTocIndexForSpineIndex(pos.spineIndex);
  const std::string chapterName = (tocIndex >= 0) ? epub->getTocItem(tocIndex).title : "unknown";

  Serial.printf("[%lu] [ProgressMapper] CrossPoint -> KOReader: chapter='%s', page=%d/%d -> %.2f%% at %s\n", millis(),
                chapterName.c_str(), pos.pageNumber, pos.totalPages, result.percentage * 100, result.xpath.c_str());

  return result;
}

CrossPointPosition ProgressMapper::toCrossPoint(const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos,
                                                int currentSpineIndex, int totalPagesInCurrentSpine) {
  CrossPointPosition result;
  result.spineIndex = 0;
  result.pageNumber = 0;
  result.totalPages = 0;

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return result;
  }

  // Use percentage-based lookup for both spine and page positioning
  // XPath parsing is unreliable since CrossPoint doesn't preserve detailed HTML structure
  const size_t targetBytes = static_cast<size_t>(bookSize * koPos.percentage);

  // Find the spine item that contains this byte position
  const int spineCount = epub->getSpineItemsCount();
  bool spineFound = false;
  for (int i = 0; i < spineCount; i++) {
    const size_t cumulativeSize = epub->getCumulativeSpineItemSize(i);
    if (cumulativeSize >= targetBytes) {
      result.spineIndex = i;
      spineFound = true;
      break;
    }
  }

  // If no spine item was found (e.g., targetBytes beyond last cumulative size),
  // default to the last spine item so we map to the end of the book instead of the beginning.
  if (!spineFound && spineCount > 0) {
    result.spineIndex = spineCount - 1;
  }

  // Estimate page number within the spine item using percentage
  if (result.spineIndex < epub->getSpineItemsCount()) {
    const size_t prevCumSize = (result.spineIndex > 0) ? epub->getCumulativeSpineItemSize(result.spineIndex - 1) : 0;
    const size_t currentCumSize = epub->getCumulativeSpineItemSize(result.spineIndex);
    const size_t spineSize = currentCumSize - prevCumSize;

    int estimatedTotalPages = 0;

    // If we are in the same spine, use the known total pages
    if (result.spineIndex == currentSpineIndex && totalPagesInCurrentSpine > 0) {
      estimatedTotalPages = totalPagesInCurrentSpine;
    }
    // Otherwise try to estimate based on density from current spine
    else if (currentSpineIndex >= 0 && currentSpineIndex < epub->getSpineItemsCount() && totalPagesInCurrentSpine > 0) {
      const size_t prevCurrCumSize =
          (currentSpineIndex > 0) ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
      const size_t currCumSize = epub->getCumulativeSpineItemSize(currentSpineIndex);
      const size_t currSpineSize = currCumSize - prevCurrCumSize;

      if (currSpineSize > 0) {
        float ratio = static_cast<float>(spineSize) / static_cast<float>(currSpineSize);
        estimatedTotalPages = static_cast<int>(totalPagesInCurrentSpine * ratio);
        if (estimatedTotalPages < 1) estimatedTotalPages = 1;
      }
    }

    result.totalPages = estimatedTotalPages;

    if (spineSize > 0 && estimatedTotalPages > 0) {
      const size_t bytesIntoSpine = (targetBytes > prevCumSize) ? (targetBytes - prevCumSize) : 0;
      const float intraSpineProgress = static_cast<float>(bytesIntoSpine) / static_cast<float>(spineSize);
      const float clampedProgress = std::max(0.0f, std::min(1.0f, intraSpineProgress));
      result.pageNumber = static_cast<int>(clampedProgress * estimatedTotalPages);
      result.pageNumber = std::max(0, std::min(result.pageNumber, estimatedTotalPages - 1));
    }
  }

  Serial.printf("[%lu] [ProgressMapper] KOReader -> CrossPoint: %.2f%% -> spine=%d, page=%d/%d\n", millis(),
                koPos.percentage * 100, result.spineIndex, result.pageNumber, result.totalPages);

  return result;
}

std::string ProgressMapper::generateXPath(int spineIndex, int pageNumber, int totalPages) {
  // Use 0-based DocFragment indices for KOReader
  // Use a simple xpath pointing to the DocFragment - KOReader will use the percentage for fine positioning within it
  // Avoid specifying paragraph numbers as they may not exist in the target document
  return "/body/DocFragment[" + std::to_string(spineIndex) + "]/body";
}
