#include "ProgressMapper.h"

#include <HardwareSerial.h>

#include <cmath>
#include <regex>

KOReaderPosition ProgressMapper::toKOReader(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos) {
  KOReaderPosition result;

  // Calculate page progress within current spine item
  float intraSpineProgress = 0.0f;
  if (pos.totalPages > 0) {
    intraSpineProgress = static_cast<float>(pos.pageNumber) / static_cast<float>(pos.totalPages);
  }

  // Calculate overall book progress (0-100 from Epub, convert to 0.0-1.0)
  const uint8_t progressPercent = epub->calculateProgress(pos.spineIndex, intraSpineProgress);
  result.percentage = static_cast<float>(progressPercent) / 100.0f;

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
                                                int totalPagesInSpine) {
  CrossPointPosition result;
  result.spineIndex = 0;
  result.pageNumber = 0;
  result.totalPages = totalPagesInSpine;

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    Serial.printf("[%lu] [ProgressMapper] Book size is 0\n", millis());
    return result;
  }

  // First, try to get spine index from XPath (DocFragment)
  int xpathSpineIndex = parseDocFragmentIndex(koPos.xpath);
  if (xpathSpineIndex >= 0 && xpathSpineIndex < epub->getSpineItemsCount()) {
    result.spineIndex = xpathSpineIndex;
    Serial.printf("[%lu] [ProgressMapper] Got spine index from XPath: %d\n", millis(), result.spineIndex);
  } else {
    // Fall back to percentage-based lookup
    const size_t targetBytes = static_cast<size_t>(bookSize * koPos.percentage);

    // Find the spine item that contains this byte position
    for (int i = 0; i < epub->getSpineItemsCount(); i++) {
      const size_t cumulativeSize = epub->getCumulativeSpineItemSize(i);
      if (cumulativeSize >= targetBytes) {
        result.spineIndex = i;
        break;
      }
    }
    Serial.printf("[%lu] [ProgressMapper] Got spine index from percentage (%.2f%%): %d\n", millis(),
                  koPos.percentage * 100, result.spineIndex);
  }

  // Estimate page number within the spine item using percentage
  if (totalPagesInSpine > 0 && result.spineIndex < epub->getSpineItemsCount()) {
    // Calculate what percentage through the spine item we should be
    const size_t prevCumSize = (result.spineIndex > 0) ? epub->getCumulativeSpineItemSize(result.spineIndex - 1) : 0;
    const size_t currentCumSize = epub->getCumulativeSpineItemSize(result.spineIndex);
    const size_t spineSize = currentCumSize - prevCumSize;

    if (spineSize > 0) {
      const size_t targetBytes = static_cast<size_t>(bookSize * koPos.percentage);
      const size_t bytesIntoSpine = (targetBytes > prevCumSize) ? (targetBytes - prevCumSize) : 0;
      const float intraSpineProgress = static_cast<float>(bytesIntoSpine) / static_cast<float>(spineSize);

      // Clamp to valid range
      const float clampedProgress = std::max(0.0f, std::min(1.0f, intraSpineProgress));
      result.pageNumber = static_cast<int>(clampedProgress * totalPagesInSpine);

      // Ensure page number is valid
      result.pageNumber = std::max(0, std::min(result.pageNumber, totalPagesInSpine - 1));
    }
  }

  Serial.printf("[%lu] [ProgressMapper] KOReader -> CrossPoint: %.2f%% at %s -> spine=%d, page=%d\n", millis(),
                koPos.percentage * 100, koPos.xpath.c_str(), result.spineIndex, result.pageNumber);

  return result;
}

std::string ProgressMapper::generateXPath(int spineIndex, int pageNumber, int totalPages) {
  // KOReader uses 1-based DocFragment indices
  // Estimate paragraph number based on page position
  // Assume ~3 paragraphs per page on average for e-reader screens
  constexpr int paragraphsPerPage = 3;
  const int estimatedParagraph = (pageNumber * paragraphsPerPage) + 1;  // 1-based

  return "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body/p[" + std::to_string(estimatedParagraph) + "]";
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
