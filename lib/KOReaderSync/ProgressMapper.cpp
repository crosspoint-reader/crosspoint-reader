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

// Compute intra-spine progress from KOReader's book percentage, assuming the target
// spine is known. This is the constrained version of resolveFromPercentage that
// honors an XPath-derived spine index even when the heavy XPath resolver couldn't
// run (typically because heap was too fragmented to inflate the chapter at sync time).
//
// The math is identical to the per-spine portion of resolveFromPercentage. Returns 0
// when the percentage maps to bytes before the spine's start (the position lives
// inside the spine by assumption, so clamp to 0) and 1 when it overshoots the end.
float intraSpineFromPercentage(const std::shared_ptr<Epub>& epub, const int spineIndex, const float percentage) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount() || !std::isfinite(percentage)) {
    return 0.0f;
  }
  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return 0.0f;
  }
  const float sanitized = std::clamp(percentage, 0.0f, 1.0f);
  const size_t targetBytes = static_cast<size_t>(bookSize * sanitized);
  const size_t prevCumSize = (spineIndex > 0) ? epub->getCumulativeSpineItemSize(spineIndex - 1) : 0;
  const size_t currentCumSize = epub->getCumulativeSpineItemSize(spineIndex);
  const size_t spineSize = currentCumSize - prevCumSize;
  if (spineSize == 0) {
    return 0.0f;
  }
  if (targetBytes <= prevCumSize) {
    return 0.0f;
  }
  const size_t bytesIntoSpine = targetBytes - prevCumSize;
  return std::clamp(static_cast<float>(bytesIntoSpine) / static_cast<float>(spineSize), 0.0f, 1.0f);
}

// KOReader emits chapter-start XPaths as ".../body/<wrapper>.0" or just
// ".../body/text()[1].0" — there's no paragraph segment, and the character offset is 0.
// These unambiguously denote "the start of the spine"; we can pin intra=0 without
// inflating the chapter. Catches the common case of starting a new chapter on
// another device, which previously round-tripped through book-percentage byte math
// and landed several pages into the chapter due to byte-vs-page-density skew.
bool isChapterStartXPath(const std::string& xpath) {
  // Reject anything with a paragraph or list-item predicate — those carry real
  // position information that can't be flattened to "start of spine".
  if (xpath.find("/p[") != std::string::npos) return false;
  if (xpath.find("/li[") != std::string::npos) return false;
  // The path must end with a ".0" text-point segment. The reverse mapper already
  // strips text() suffixes for matching, but here we look at the raw form: either
  // "<tag>.0" (cursor at start of element) or "text()[1].0" / similar (cursor at
  // start of the first text node) with no following character offset.
  const size_t dotPos = xpath.rfind('.');
  if (dotPos == std::string::npos || dotPos + 1 >= xpath.size()) return false;
  for (size_t i = dotPos + 1; i < xpath.size(); i++) {
    if (xpath[i] != '0') return false;
  }
  return true;
}
}  // namespace

KOReaderPosition ProgressMapper::toKOReader(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos) {
  KOReaderPosition result;

  // Calculate page progress within current spine item.
  // Page numbers are 0-based and totalPages is 1-based, so the last page is
  // (totalPages - 1). Dividing by (totalPages - 1) maps page 0 to intra=0 and the
  // last page to intra=1, which is what KOReader expects when round-tripping.
  // Dividing by totalPages would peg the last page short of 100%, so a user who
  // finished the chapter would only show ~94% on KOReader.
  float intraSpineProgress = 0.0f;
  if (pos.totalPages > 1) {
    intraSpineProgress = static_cast<float>(pos.pageNumber) / static_cast<float>(pos.totalPages - 1);
  } else if (pos.totalPages == 1) {
    intraSpineProgress = 0.0f;
  }

  // Calculate overall book progress (0.0-1.0)
  result.percentage = epub->calculateProgress(pos.spineIndex, intraSpineProgress);

  // Generate XPath for the current position via byte-offset scan. Targeting the
  // paragraph LUT entry instead would snap to the start of the paragraph the user
  // is inside, which causes pulled positions to land at the start of the chapter
  // when an opening paragraph spans many pages.
  result.xpath = ChapterXPathIndexer::findXPathForProgress(epub, pos.spineIndex, intraSpineProgress);
  if (result.xpath.empty()) {
    result.xpath = generateXPath(pos.spineIndex);
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

  // Mapping source used for the final log line; updated as we narrow down the path
  // actually taken (xpath / xpath+percentage / xpath-spine+percentage / percentage).
  const char* mappingSource = "percentage";

  int xpathSpineIndex = -1;
  const bool haveXPathSpine = ChapterXPathIndexer::tryExtractSpineIndexFromXPath(koPos.xpath, xpathSpineIndex) &&
                              xpathSpineIndex >= 0 && xpathSpineIndex < spineCount;
  if (haveXPathSpine) {
    float intraFromXPath = 0.0f;
    uint16_t liIndexFromXPath = 0;
    if (ChapterXPathIndexer::findProgressForXPath(epub, xpathSpineIndex, koPos.xpath, intraFromXPath, xpathExactMatch,
                                                  &liIndexFromXPath)) {
      result.spineIndex = xpathSpineIndex;
      resolvedIntraSpineProgress = intraFromXPath;
      usedXPathMapping = true;
      if (liIndexFromXPath > 0) {
        result.listItemIndex = liIndexFromXPath;
        result.hasListItemIndex = true;
      }

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
      mappingSource = usedPercentageReconcile ? "xpath+percentage" : "xpath";
    }
    // Extract paragraph index from XPath for direct page lookup via section cache.
    // Done regardless of whether the heavy XPath resolver succeeded — the paragraph
    // LUT lookup later (in EpubReaderActivity::NavigationTarget::resolveInto) snaps
    // to the precise page, so even without intra resolution we get an exact landing.
    uint16_t pIndex = 0;
    if (ChapterXPathIndexer::tryExtractParagraphIndexFromXPath(koPos.xpath, pIndex)) {
      result.paragraphIndex = pIndex;
      result.hasParagraphIndex = true;
    }
  }

  if (!usedXPathMapping) {
    // Heavy XPath resolution failed (typically because heap was too fragmented to
    // inflate the spine at sync time). Salvage as much as we can:
    //   1) Trust the spine index extracted from the XPath itself — it's purely
    //      string-derived and always correct when present. Using it preserves
    //      cross-chapter syncs even when chapter content can't be re-parsed.
    //   2) For chapter-start XPaths (ending in ".0" with no paragraph predicate),
    //      pin intra=0. KOReader's percentage carries small per-DOM rounding that
    //      would otherwise leak into a spurious intra > 0 via byte-fraction math.
    //   3) Otherwise compute intra-spine from KOReader's percentage relative to
    //      the XPath-derived spine. Falls back to global percentage spine selection
    //      only when no XPath spine is available.
    if (haveXPathSpine) {
      result.spineIndex = xpathSpineIndex;
      if (isChapterStartXPath(koPos.xpath)) {
        resolvedIntraSpineProgress = 0.0f;
        mappingSource = "xpath-spine+chapter-start";
        LOG_DBG("ProgressMapper", "Chapter-start XPath '%s' on spine=%d, pinning intra=0", koPos.xpath.c_str(),
                xpathSpineIndex);
      } else {
        resolvedIntraSpineProgress = intraSpineFromPercentage(epub, xpathSpineIndex, koPos.percentage);
        mappingSource = "xpath-spine+percentage";
        LOG_DBG("ProgressMapper", "XPath resolve unavailable for spine=%d; intra from pct=%.3f -> %.3f",
                xpathSpineIndex, koPos.percentage, resolvedIntraSpineProgress);
      }
    } else {
      int percentageSpineIndex = -1;
      float percentageIntraSpine = -1.0f;
      if (!resolveFromPercentage(epub, koPos.percentage, spineCount, percentageSpineIndex, percentageIntraSpine)) {
        return result;
      }
      result.spineIndex = percentageSpineIndex;
      resolvedIntraSpineProgress = percentageIntraSpine;
    }
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
      // Symmetric inverse of the toKOReader formula: intra=1.0 should land on the
      // last page (totalPages - 1), intra=0 on page 0. Round-to-nearest avoids
      // truncating mid-page progress down to the previous page.
      if (estimatedTotalPages > 1) {
        result.pageNumber = static_cast<int>(clampedProgress * static_cast<float>(estimatedTotalPages - 1) + 0.5f);
      } else {
        result.pageNumber = 0;
      }
      result.pageNumber = std::max(0, std::min(result.pageNumber, estimatedTotalPages - 1));
    } else if (spineSize > 0 && estimatedTotalPages > 0) {
      result.pageNumber = 0;
    }
  }

  LOG_DBG("ProgressMapper", "Resolved KOReader position: spine=%d intra=%.3f hasPIdx=%s pIdx=%u hasLiIdx=%s liIdx=%u",
          result.spineIndex, resolvedIntraSpineProgress, result.hasParagraphIndex ? "yes" : "no", result.paragraphIndex,
          result.hasListItemIndex ? "yes" : "no", result.listItemIndex);

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
