#include "ProgressMapper.h"

#include <Logging.h>

#include <algorithm>
#include <cmath>

#include "ChapterXPathIndexer.h"

namespace {
bool resolveFromPercentage(const std::shared_ptr<Epub>& epub, const float percentage, const int spineCount,
                           int& outSpineIndex, float& outIntraSpineProgress) {
  if (!std::isfinite(percentage) || !epub || spineCount <= 0) {
    return false;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return false;
  }

  const float sanitizedPercentage = std::clamp(percentage, 0.0f, 1.0f);
  const size_t targetBytes = static_cast<size_t>(bookSize * sanitizedPercentage);

  outSpineIndex = spineCount - 1;
  for (int i = 0; i < spineCount; i++) {
    const size_t cumulativeSize = epub->getCumulativeSpineItemSize(i);
    if (cumulativeSize >= targetBytes) {
      outSpineIndex = i;
      break;
    }
  }

  outIntraSpineProgress = 0.0f;
  const size_t prevCumSize = (outSpineIndex > 0) ? epub->getCumulativeSpineItemSize(outSpineIndex - 1) : 0;
  const size_t currentCumSize = epub->getCumulativeSpineItemSize(outSpineIndex);
  const size_t spineSize = currentCumSize - prevCumSize;
  if (spineSize > 0) {
    const size_t bytesIntoSpine = (targetBytes > prevCumSize) ? (targetBytes - prevCumSize) : 0;
    outIntraSpineProgress = static_cast<float>(bytesIntoSpine) / static_cast<float>(spineSize);
    outIntraSpineProgress = std::clamp(outIntraSpineProgress, 0.0f, 1.0f);
  }

  return true;
}
}  // namespace

KOReaderPosition ProgressMapper::toKOReader(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos) {
  KOReaderPosition result;

  // Calculate page progress within current spine item
  float intraSpineProgress = 0.0f;
  if (pos.totalPages > 0) {
    intraSpineProgress = static_cast<float>(pos.pageNumber) / static_cast<float>(pos.totalPages);
  }

  // Calculate overall book progress (0.0-1.0)
  result.percentage = epub->calculateProgress(pos.spineIndex, intraSpineProgress);

  // Generate XPath for the current position.
  // Prefer paragraph index from the section cache LUT (exact element mapping) over
  // byte-offset estimation (which can drift in chapters with non-uniform content density).
  if (pos.hasParagraphIndex && pos.paragraphIndex > 0) {
    result.xpath = "/body/DocFragment[" + std::to_string(pos.spineIndex + 1) + "]/body/p[" +
                   std::to_string(pos.paragraphIndex) + "]";
  } else {
    result.xpath = ChapterXPathIndexer::findXPathForProgress(epub, pos.spineIndex, intraSpineProgress);
    if (result.xpath.empty()) {
      result.xpath = generateXPath(pos.spineIndex);
    }
  }

  // Get chapter info for logging
  const int tocIndex = epub->getTocIndexForSpineIndex(pos.spineIndex);
  const std::string chapterName = (tocIndex >= 0) ? epub->getTocItem(tocIndex).title : "unknown";

  LOG_DBG("ProgressMapper", "CrossPoint -> KOReader: chapter='%s', page=%d/%d -> %.2f%% at %s", chapterName.c_str(),
          pos.pageNumber, pos.totalPages, result.percentage * 100, result.xpath.c_str());

  return result;
}

CrossPointPosition ProgressMapper::toCrossPoint(const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos,
                                                int currentSpineIndex, int totalPagesInCurrentSpine) {
  CrossPointPosition result;
  result.spineIndex = 0;
  result.pageNumber = 0;
  result.totalPages = 0;

  if (!epub || epub->getSpineItemsCount() <= 0) {
    return result;
  }

  const int spineCount = epub->getSpineItemsCount();

  float resolvedIntraSpineProgress = -1.0f;
  bool xpathExactMatch = false;
  bool usedXPathMapping = false;
  bool usedPercentageReconcile = false;

  int xpathSpineIndex = -1;
  if (ChapterXPathIndexer::tryExtractSpineIndexFromXPath(koPos.xpath, xpathSpineIndex) && xpathSpineIndex >= 0 &&
      xpathSpineIndex < spineCount) {
    float intraFromXPath = 0.0f;
    if (ChapterXPathIndexer::findProgressForXPath(epub, xpathSpineIndex, koPos.xpath, intraFromXPath,
                                                  xpathExactMatch)) {
      result.spineIndex = xpathSpineIndex;
      resolvedIntraSpineProgress = intraFromXPath;
      usedXPathMapping = true;

      // KOReader's text-node indexing can differ across renderers/parsers in some
      // XHTML shapes. When an XPath-resolved position disagrees materially with
      // KOReader's percentage but points to the same spine, use percentage-derived
      // intra-spine progress as a safer tie-breaker.
      if (std::isfinite(koPos.percentage) && resolvedIntraSpineProgress >= 0.0f) {
        const float sanitizedPercentage = std::clamp(koPos.percentage, 0.0f, 1.0f);
        const float mappedPercentage = epub->calculateProgress(result.spineIndex, resolvedIntraSpineProgress);
        const float delta = std::fabs(mappedPercentage - sanitizedPercentage);

        constexpr float kReconcileThreshold = 0.01f;  // 1% absolute book progress
        if (delta > kReconcileThreshold) {
          int percentageSpineIndex = -1;
          float percentageIntraSpine = -1.0f;
          if (resolveFromPercentage(epub, koPos.percentage, spineCount, percentageSpineIndex, percentageIntraSpine) &&
              percentageSpineIndex == result.spineIndex && percentageIntraSpine >= 0.0f) {
            LOG_DBG("ProgressMapper",
                    "Reconciling XPath position with percentage: spine=%d xpath=%.3f pct=%.3f delta=%.3f -> %.3f",
                    result.spineIndex, resolvedIntraSpineProgress, sanitizedPercentage, delta, percentageIntraSpine);
            resolvedIntraSpineProgress = percentageIntraSpine;
            usedPercentageReconcile = true;
          }
        }
      }
    }
    // Extract paragraph index from XPath for direct page lookup via section cache
    uint16_t pIndex = 0;
    if (ChapterXPathIndexer::tryExtractParagraphIndexFromXPath(koPos.xpath, pIndex)) {
      result.paragraphIndex = pIndex;
      result.hasParagraphIndex = true;
    }
  }

  if (!usedXPathMapping) {
    int percentageSpineIndex = -1;
    float percentageIntraSpine = -1.0f;
    if (!resolveFromPercentage(epub, koPos.percentage, spineCount, percentageSpineIndex, percentageIntraSpine)) {
      return result;
    }

    result.spineIndex = percentageSpineIndex;
    resolvedIntraSpineProgress = percentageIntraSpine;
  }

  // Estimate page number within the selected spine item
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

    if (estimatedTotalPages > 0 && resolvedIntraSpineProgress >= 0.0f) {
      const float clampedProgress = std::max(0.0f, std::min(1.0f, resolvedIntraSpineProgress));
      result.pageNumber = static_cast<int>(clampedProgress * static_cast<float>(estimatedTotalPages));
      result.pageNumber = std::max(0, std::min(result.pageNumber, estimatedTotalPages - 1));
    } else if (spineSize > 0 && estimatedTotalPages > 0) {
      result.pageNumber = 0;
    }
  }

  LOG_DBG("ProgressMapper", "Resolved KOReader position: spine=%d intra=%.3f hasPIdx=%s pIdx=%u", result.spineIndex,
          resolvedIntraSpineProgress, result.hasParagraphIndex ? "yes" : "no", result.paragraphIndex);

  const char* mappingSource =
      usedXPathMapping ? (usedPercentageReconcile ? "xpath+percentage" : "xpath") : "percentage";
  LOG_DBG("ProgressMapper", "KOReader -> CrossPoint: %.2f%% at %s -> spine=%d, page=%d (%s, exact=%s)",
          koPos.percentage * 100, koPos.xpath.c_str(), result.spineIndex, result.pageNumber, mappingSource,
          xpathExactMatch ? "yes" : "no");

  return result;
}

std::string ProgressMapper::generateXPath(int spineIndex) {
  // Fallback path when element-level XPath extraction is unavailable.
  // KOReader uses 1-based XPath predicates; spineIndex is 0-based internally.
  return "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
}
