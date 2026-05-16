#pragma once

#include <cstdint>

#include "EpubIndexingPolicy.h"

enum class IncrementalBuildBudgetProfile {
  InitialIndex,
  CurrentBackground,
  NextPrewarm,
  Outrun,
};

struct IncrementalBuildBudget {
  uint16_t maxInputChunks = 0;
  uint16_t maxCompletedPages = 1;
  uint32_t maxMillis = 20;
  uint32_t minFreeHeap = EpubIndexingPolicy::ACTIVE_INCREMENTAL_MIN_FREE_HEAP;
};

class IncrementalBuildBudgets {
 public:
  static constexpr IncrementalBuildBudget forProfile(const IncrementalBuildBudgetProfile profile) {
    switch (profile) {
      case IncrementalBuildBudgetProfile::InitialIndex:
        return standard(EpubIndexingPolicy::INITIAL_INDEX_BATCH_PAGES,
                        EpubIndexingPolicy::FOREGROUND_INCREMENTAL_MIN_FREE_HEAP);
      case IncrementalBuildBudgetProfile::CurrentBackground:
        return standard(EpubIndexingPolicy::CURRENT_INDEX_BATCH_PAGES,
                        EpubIndexingPolicy::ACTIVE_INCREMENTAL_MIN_FREE_HEAP);
      case IncrementalBuildBudgetProfile::NextPrewarm:
        return {EpubIndexingPolicy::nextPrewarmInputChunks(EpubIndexingPolicy::NEXT_PREWARM_BATCH_PAGES),
                EpubIndexingPolicy::clampIndexBatchPages(EpubIndexingPolicy::NEXT_PREWARM_BATCH_PAGES),
                EpubIndexingPolicy::indexBatchMaxMillis(EpubIndexingPolicy::NEXT_PREWARM_BATCH_PAGES),
                EpubIndexingPolicy::ACTIVE_INCREMENTAL_MIN_FREE_HEAP};
      case IncrementalBuildBudgetProfile::Outrun:
        return standard(EpubIndexingPolicy::OUTRUN_INDEX_BATCH_PAGES,
                        EpubIndexingPolicy::FOREGROUND_INCREMENTAL_MIN_FREE_HEAP);
    }
    return standard(EpubIndexingPolicy::CURRENT_INDEX_BATCH_PAGES,
                    EpubIndexingPolicy::ACTIVE_INCREMENTAL_MIN_FREE_HEAP);
  }

 private:
  static constexpr IncrementalBuildBudget standard(const uint16_t pages, const uint32_t minFreeHeap) {
    return {EpubIndexingPolicy::indexBatchInputChunks(pages), EpubIndexingPolicy::clampIndexBatchPages(pages),
            EpubIndexingPolicy::indexBatchMaxMillis(pages), minFreeHeap};
  }
};
