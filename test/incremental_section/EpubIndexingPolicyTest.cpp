#include <cassert>

#include "../../lib/Epub/Epub/EpubIndexingPolicy.h"
#include "../../lib/Epub/Epub/IncrementalBuildBudget.h"

static_assert(EpubIndexingPolicy::CURRENT_INDEX_BATCH_PAGES == 1);
static_assert(EpubIndexingPolicy::INITIAL_INDEX_BATCH_PAGES == 1);
static_assert(EpubIndexingPolicy::INITIAL_INDEX_TARGET_PAGES == 1);
static_assert(EpubIndexingPolicy::OUTRUN_INDEX_BATCH_PAGES == 1);
static_assert(EpubIndexingPolicy::NEXT_PREWARM_BATCH_PAGES == 1);
static_assert(EpubIndexingPolicy::INDEX_BATCH_MIN_PAGES == 1);
static_assert(EpubIndexingPolicy::INDEX_BATCH_MAX_PAGES == 10);
static_assert(EpubIndexingPolicy::CURRENT_INDEX_MIN_INTERVAL_MS == 1000);
static_assert(EpubIndexingPolicy::NEXT_PREWARM_MIN_INTERVAL_MS == 1500);

static_assert(EpubIndexingPolicy::clampIndexBatchPages(0) == 1);
static_assert(EpubIndexingPolicy::clampIndexBatchPages(6) == 6);
static_assert(EpubIndexingPolicy::clampIndexBatchPages(11) == 10);

static_assert(EpubIndexingPolicy::indexBatchInputChunks(EpubIndexingPolicy::CURRENT_INDEX_BATCH_PAGES) == 1);
static_assert(EpubIndexingPolicy::indexBatchInputChunks(EpubIndexingPolicy::INITIAL_INDEX_BATCH_PAGES) == 1);
static_assert(EpubIndexingPolicy::indexBatchInputChunks(EpubIndexingPolicy::OUTRUN_INDEX_BATCH_PAGES) == 1);
static_assert(EpubIndexingPolicy::nextPrewarmInputChunks(EpubIndexingPolicy::NEXT_PREWARM_BATCH_PAGES) == 3);

static_assert(EpubIndexingPolicy::indexBatchMaxMillis(EpubIndexingPolicy::CURRENT_INDEX_BATCH_PAGES) == 50);
static_assert(EpubIndexingPolicy::indexBatchMaxMillis(EpubIndexingPolicy::INITIAL_INDEX_BATCH_PAGES) == 50);
static_assert(EpubIndexingPolicy::indexBatchMaxMillis(EpubIndexingPolicy::OUTRUN_INDEX_BATCH_PAGES) == 50);

static_assert(EpubIndexingPolicy::initialIndexWindowNeedsPages(0, false));
static_assert(!EpubIndexingPolicy::initialIndexWindowNeedsPages(1, false));
static_assert(!EpubIndexingPolicy::initialIndexWindowNeedsPages(0, true));
static_assert(EpubIndexingPolicy::outrunTargetKnownPages(22) == 23);
static_assert(EpubIndexingPolicy::outrunIndexWindowNeedsPages(22, 22, false));
static_assert(!EpubIndexingPolicy::outrunIndexWindowNeedsPages(22, 23, false));
static_assert(!EpubIndexingPolicy::outrunIndexWindowNeedsPages(22, 23, true));

static_assert(EpubIndexingPolicy::shouldPrewarmNextChapter(0, 10, true));
static_assert(EpubIndexingPolicy::shouldPrewarmNextChapter(8, 10, true));
static_assert(!EpubIndexingPolicy::shouldPrewarmNextChapter(8, 10, false));
static_assert(!EpubIndexingPolicy::shouldPrewarmNextChapter(0, 0, true));
static_assert(EpubIndexingPolicy::prewarmMatchesSpine(2, 2));
static_assert(!EpubIndexingPolicy::prewarmMatchesSpine(1, 2));

constexpr auto initialBudget = IncrementalBuildBudgets::forProfile(IncrementalBuildBudgetProfile::InitialIndex);
constexpr auto currentBudget = IncrementalBuildBudgets::forProfile(IncrementalBuildBudgetProfile::CurrentBackground);
constexpr auto prewarmBudget = IncrementalBuildBudgets::forProfile(IncrementalBuildBudgetProfile::NextPrewarm);
constexpr auto outrunBudget = IncrementalBuildBudgets::forProfile(IncrementalBuildBudgetProfile::Outrun);

static_assert(initialBudget.maxInputChunks == 1);
static_assert(initialBudget.maxCompletedPages == 1);
static_assert(initialBudget.maxMillis == 50);
static_assert(currentBudget.maxInputChunks == 1);
static_assert(currentBudget.maxCompletedPages == 1);
static_assert(currentBudget.maxMillis == 50);
static_assert(prewarmBudget.maxInputChunks == 3);
static_assert(prewarmBudget.maxCompletedPages == 1);
static_assert(prewarmBudget.maxMillis == 50);
static_assert(outrunBudget.maxInputChunks == 1);
static_assert(outrunBudget.maxCompletedPages == 1);
static_assert(outrunBudget.maxMillis == 50);

int main() {
  assert(EpubIndexingPolicy::hasHeapForForegroundIncrementalPump(
      EpubIndexingPolicy::FOREGROUND_INCREMENTAL_MIN_FREE_HEAP));
  assert(!EpubIndexingPolicy::hasHeapForForegroundIncrementalPump(
      EpubIndexingPolicy::FOREGROUND_INCREMENTAL_MIN_FREE_HEAP - 1));
  assert(EpubIndexingPolicy::hasHeapForActiveIncrementalPump(EpubIndexingPolicy::ACTIVE_INCREMENTAL_MIN_FREE_HEAP));
  assert(!EpubIndexingPolicy::hasHeapForEmbeddedStyle(EpubIndexingPolicy::EMBEDDED_STYLE_MIN_FREE_HEAP - 1));
  assert(EpubIndexingPolicy::hasHeapForEmbeddedStyle(EpubIndexingPolicy::EMBEDDED_STYLE_MIN_FREE_HEAP));
}
