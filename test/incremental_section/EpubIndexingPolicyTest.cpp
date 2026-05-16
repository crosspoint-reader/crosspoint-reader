#include "../../lib/Epub/Epub/EpubIndexingPolicy.h"
#include "../../lib/Epub/Epub/IncrementalBuildBudget.h"

#include <cassert>

static_assert(EpubIndexingPolicy::CURRENT_INDEX_BATCH_PAGES == 10);
static_assert(EpubIndexingPolicy::INITIAL_INDEX_BATCH_PAGES == 3);
static_assert(EpubIndexingPolicy::INITIAL_INDEX_TARGET_PAGES == 3);
static_assert(EpubIndexingPolicy::OUTRUN_INDEX_BATCH_PAGES == 5);
static_assert(EpubIndexingPolicy::NEXT_PREWARM_BATCH_PAGES == 3);
static_assert(EpubIndexingPolicy::INDEX_BATCH_MIN_PAGES == 1);
static_assert(EpubIndexingPolicy::INDEX_BATCH_MAX_PAGES == 10);
static_assert(EpubIndexingPolicy::CURRENT_INDEX_MIN_INTERVAL_MS == 1000);
static_assert(EpubIndexingPolicy::NEXT_PREWARM_MIN_INTERVAL_MS == 1500);

static_assert(EpubIndexingPolicy::clampIndexBatchPages(0) == 1);
static_assert(EpubIndexingPolicy::clampIndexBatchPages(6) == 6);
static_assert(EpubIndexingPolicy::clampIndexBatchPages(11) == 10);

static_assert(EpubIndexingPolicy::indexBatchInputChunks(EpubIndexingPolicy::CURRENT_INDEX_BATCH_PAGES) == 10);
static_assert(EpubIndexingPolicy::indexBatchInputChunks(EpubIndexingPolicy::INITIAL_INDEX_BATCH_PAGES) == 3);
static_assert(EpubIndexingPolicy::indexBatchInputChunks(EpubIndexingPolicy::OUTRUN_INDEX_BATCH_PAGES) == 5);
static_assert(EpubIndexingPolicy::nextPrewarmInputChunks(EpubIndexingPolicy::NEXT_PREWARM_BATCH_PAGES) == 9);

static_assert(EpubIndexingPolicy::indexBatchMaxMillis(EpubIndexingPolicy::CURRENT_INDEX_BATCH_PAGES) == 500);
static_assert(EpubIndexingPolicy::indexBatchMaxMillis(EpubIndexingPolicy::INITIAL_INDEX_BATCH_PAGES) == 150);
static_assert(EpubIndexingPolicy::indexBatchMaxMillis(EpubIndexingPolicy::OUTRUN_INDEX_BATCH_PAGES) == 250);

static_assert(EpubIndexingPolicy::initialIndexWindowNeedsPages(0, false));
static_assert(EpubIndexingPolicy::initialIndexWindowNeedsPages(2, false));
static_assert(!EpubIndexingPolicy::initialIndexWindowNeedsPages(3, false));
static_assert(!EpubIndexingPolicy::initialIndexWindowNeedsPages(0, true));
static_assert(EpubIndexingPolicy::outrunTargetKnownPages(22) == 27);
static_assert(EpubIndexingPolicy::outrunIndexWindowNeedsPages(22, 26, false));
static_assert(!EpubIndexingPolicy::outrunIndexWindowNeedsPages(22, 27, false));
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

static_assert(initialBudget.maxInputChunks == 3);
static_assert(initialBudget.maxCompletedPages == 3);
static_assert(initialBudget.maxMillis == 150);
static_assert(currentBudget.maxInputChunks == 10);
static_assert(currentBudget.maxCompletedPages == 10);
static_assert(currentBudget.maxMillis == 500);
static_assert(prewarmBudget.maxInputChunks == 9);
static_assert(prewarmBudget.maxCompletedPages == 3);
static_assert(outrunBudget.maxInputChunks == 5);
static_assert(outrunBudget.maxCompletedPages == 5);

int main() {
  assert(EpubIndexingPolicy::hasHeapForForegroundIncrementalPump(
      EpubIndexingPolicy::FOREGROUND_INCREMENTAL_MIN_FREE_HEAP));
  assert(!EpubIndexingPolicy::hasHeapForForegroundIncrementalPump(
      EpubIndexingPolicy::FOREGROUND_INCREMENTAL_MIN_FREE_HEAP - 1));
  assert(EpubIndexingPolicy::hasHeapForActiveIncrementalPump(EpubIndexingPolicy::ACTIVE_INCREMENTAL_MIN_FREE_HEAP));
  assert(!EpubIndexingPolicy::hasHeapForEmbeddedStyle(EpubIndexingPolicy::EMBEDDED_STYLE_MIN_FREE_HEAP - 1));
  assert(EpubIndexingPolicy::hasHeapForEmbeddedStyle(EpubIndexingPolicy::EMBEDDED_STYLE_MIN_FREE_HEAP));
}
