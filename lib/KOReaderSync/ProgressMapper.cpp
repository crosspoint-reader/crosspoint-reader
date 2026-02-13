#include "ProgressMapper.h"

#include <Logging.h>

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

  LOG_DBG("ProgressMapper", "CrossPoint -> KOReader: chapter='%s', page=%d/%d -> %.2f%% at %s", chapterName.c_str(),
          pos.pageNumber, pos.totalPages, result.percentage * 100, result.xpath.c_str());

  return result;
}

CrossPointPosition ProgressMapper::toCrossPoint(const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos,
                                                int totalPagesInSpine) {
  CrossPointPosition result;
  result.spineIndex = 0;
  result.pageNumber = 0;
  result.totalPages = totalPagesInSpine;

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return result;
  }

  // First, try to get spine index from XPath (DocFragment)
  int xpathSpineIndex = parseDocFragmentIndex(koPos.xpath);
  if (xpathSpineIndex >= 0 && xpathSpineIndex < epub->getSpineItemsCount()) {
    result.spineIndex = xpathSpineIndex;
    // When we have XPath, go to page 0 of the spine - byte-based page calculation is unreliable
    result.pageNumber = 0;
  } else {
    // Fall back to percentage-based lookup for both spine and page
    const size_t targetBytes = static_cast<size_t>(bookSize * koPos.percentage);

    // Find the spine item that contains this byte position
    for (int i = 0; i < epub->getSpineItemsCount(); i++) {
      const size_t cumulativeSize = epub->getCumulativeSpineItemSize(i);
      if (cumulativeSize >= targetBytes) {
        result.spineIndex = i;
        break;
      }
    }

    // Estimate page number within the spine item using percentage (only when no XPath)
    if (totalPagesInSpine > 0 && result.spineIndex < epub->getSpineItemsCount()) {
      const size_t prevCumSize = (result.spineIndex > 0) ? epub->getCumulativeSpineItemSize(result.spineIndex - 1) : 0;
      const size_t currentCumSize = epub->getCumulativeSpineItemSize(result.spineIndex);
      const size_t spineSize = currentCumSize - prevCumSize;

      if (spineSize > 0) {
        const size_t bytesIntoSpine = (targetBytes > prevCumSize) ? (targetBytes - prevCumSize) : 0;
        const float intraSpineProgress = static_cast<float>(bytesIntoSpine) / static_cast<float>(spineSize);
        const float clampedProgress = std::max(0.0f, std::min(1.0f, intraSpineProgress));
        result.pageNumber = static_cast<int>(clampedProgress * totalPagesInSpine);
        result.pageNumber = std::max(0, std::min(result.pageNumber, totalPagesInSpine - 1));
      }
    }
  }

  LOG_DBG("ProgressMapper", "KOReader -> CrossPoint: %.2f%% at %s -> spine=%d, page=%d", koPos.percentage * 100,
          koPos.xpath.c_str(), result.spineIndex, result.pageNumber);

  return result;
}

std::string ProgressMapper::generateXPath(int spineIndex, int pageNumber, int totalPages) {
  // KOReader uses 1-based DocFragment indices
  // Use a simple xpath pointing to the DocFragment - KOReader will use the percentage for fine positioning
  // Avoid specifying paragraph numbers as they may not exist in the target document
  return "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
}

int ProgressMapper::parseDocFragmentIndex(const std::string& xpath) {
  // Look for DocFragment[N] pattern
  const size_t start = xpath.find("DocFragment[");
  if (start == std::string::npos) {
    return -1;
  }

  const size_t numStart = start + 12;  // Length of "DocFragment["
  const size_t numEnd = xpath.find(']', numStart);
  if (numEnd == std::string::npos) {
    return -1;
  }

  try {
    const int docFragmentIndex = std::stoi(xpath.substr(numStart, numEnd - numStart));
    // KOReader uses 1-based indices, we use 0-based
    return docFragmentIndex - 1;
  } catch (...) {
    return -1;
  }
}
