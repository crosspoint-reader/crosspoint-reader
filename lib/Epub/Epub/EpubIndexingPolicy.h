#pragma once

#include <cstdint>

class EpubIndexingPolicy {
 public:
  static constexpr uint16_t INDEX_BATCH_MIN_PAGES = 1;
  static constexpr uint16_t INDEX_BATCH_MAX_PAGES = 10;
  static constexpr uint16_t CURRENT_INDEX_BATCH_PAGES = 10;
  static constexpr uint16_t INITIAL_INDEX_BATCH_PAGES = 3;
  static constexpr uint16_t INITIAL_INDEX_TARGET_PAGES = 3;
  static constexpr uint16_t OUTRUN_INDEX_BATCH_PAGES = 5;
  static constexpr uint16_t NEXT_PREWARM_BATCH_PAGES = 3;
  static constexpr uint16_t NEXT_PREWARM_INPUT_CHUNKS_PER_PAGE = 3;
  static constexpr uint32_t INDEX_BATCH_MS_PER_PAGE = 50;
  static constexpr uint32_t CURRENT_INDEX_MIN_INTERVAL_MS = 1000;
  static constexpr uint32_t NEXT_PREWARM_MIN_INTERVAL_MS = 1500;
  static constexpr uint32_t FOREGROUND_INCREMENTAL_MIN_FREE_HEAP = 12 * 1024;
  static constexpr uint32_t ACTIVE_INCREMENTAL_MIN_FREE_HEAP = 16 * 1024;
  static constexpr uint32_t EMBEDDED_STYLE_MIN_FREE_HEAP = 48 * 1024;

  static constexpr uint16_t clampIndexBatchPages(const uint16_t pages) {
    if (pages < INDEX_BATCH_MIN_PAGES) {
      return INDEX_BATCH_MIN_PAGES;
    }
    if (pages > INDEX_BATCH_MAX_PAGES) {
      return INDEX_BATCH_MAX_PAGES;
    }
    return pages;
  }

  static constexpr uint16_t indexBatchInputChunks(const uint16_t pages) { return clampIndexBatchPages(pages); }

  static constexpr uint16_t nextPrewarmInputChunks(const uint16_t pages) {
    return static_cast<uint16_t>(clampIndexBatchPages(pages) * NEXT_PREWARM_INPUT_CHUNKS_PER_PAGE);
  }

  static constexpr uint32_t indexBatchMaxMillis(const uint16_t pages) {
    return static_cast<uint32_t>(clampIndexBatchPages(pages)) * INDEX_BATCH_MS_PER_PAGE;
  }

  static constexpr bool initialIndexWindowNeedsPages(const uint32_t knownPages, const bool complete) {
    return !complete && knownPages < INITIAL_INDEX_TARGET_PAGES;
  }

  static constexpr uint32_t outrunTargetKnownPages(const uint32_t targetPage) {
    return targetPage + static_cast<uint32_t>(clampIndexBatchPages(OUTRUN_INDEX_BATCH_PAGES));
  }

  static constexpr bool outrunIndexWindowNeedsPages(const uint32_t targetPage, const uint32_t knownPages,
                                                   const bool complete) {
    return !complete && knownPages < outrunTargetKnownPages(targetPage);
  }

  static constexpr bool shouldPrewarmNextChapter(const uint32_t currentPage, const uint32_t finalPageCount,
                                                 const bool complete) {
    (void)currentPage;
    return complete && finalPageCount > 0;
  }

  static constexpr bool prewarmMatchesSpine(const int prewarmSpineIndex, const int targetSpineIndex) {
    return prewarmSpineIndex == targetSpineIndex;
  }

  static constexpr bool hasHeapForActiveIncrementalPump(const uint32_t freeHeap) {
    return freeHeap >= ACTIVE_INCREMENTAL_MIN_FREE_HEAP;
  }

  static constexpr bool hasHeapForForegroundIncrementalPump(const uint32_t freeHeap) {
    return freeHeap >= FOREGROUND_INCREMENTAL_MIN_FREE_HEAP;
  }

  static constexpr bool hasHeapForEmbeddedStyle(const uint32_t freeHeap) {
    return freeHeap >= EMBEDDED_STYLE_MIN_FREE_HEAP;
  }
};
