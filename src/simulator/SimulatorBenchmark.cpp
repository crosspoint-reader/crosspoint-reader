#ifdef SIMULATOR

#include "SimulatorBenchmark.h"

#include <Logging.h>

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "activities/ActivityManager.h"
#include "activities/reader/EpubReaderActivity.h"
#include "simulator/SimulatorHeap.h"

namespace {

constexpr std::size_t kMedianHistogramMs = 10000;
constexpr std::size_t kMedianOverflowBucket = kMedianHistogramMs;
constexpr std::size_t kMedianBucketCount = kMedianHistogramMs + 1;

struct BenchmarkState {
  bool enabled = false;
  bool clearCache = false;
  bool started = false;
  bool finished = false;
  std::string epubPath;
  int targetPages = 100;
  int startSpine = -1;
  int startPage = 0;
  unsigned long initialLoadMs = 0;
  unsigned long startMs = 0;
  std::size_t pageTurns = 0;
  unsigned long totalPageTurnMs = 0;
  unsigned long maxPageTurnMs = 0;
  std::array<std::size_t, kMedianBucketCount> pageTurnHistogram = {};
};

BenchmarkState gState;

int parsePositiveIntEnv(const char* name, const int fallback) {
  const char* raw = std::getenv(name);
  if (!raw || !*raw) return fallback;

  char* end = nullptr;
  const long parsed = std::strtol(raw, &end, 10);
  if (!end || *end != '\0' || parsed <= 0) {
    LOG_ERR("SIMBENCH", "Invalid %s=%s, using %d", name, raw, fallback);
    return fallback;
  }
  return static_cast<int>(parsed);
}

int parseNonNegativeIntEnv(const char* name, const int fallback) {
  const char* raw = std::getenv(name);
  if (!raw || !*raw) return fallback;

  char* end = nullptr;
  const long parsed = std::strtol(raw, &end, 10);
  if (!end || *end != '\0' || parsed < 0) {
    LOG_ERR("SIMBENCH", "Invalid %s=%s, using %d", name, raw, fallback);
    return fallback;
  }
  return static_cast<int>(parsed);
}

bool parseBoolEnv(const char* name) {
  const char* raw = std::getenv(name);
  if (!raw || !*raw) return false;
  return std::string(raw) != "0";
}

EpubReaderActivity* currentReader() {
  return dynamic_cast<EpubReaderActivity*>(activityManager.getCurrentActivityForSimulator());
}

void printSummary() {
  const unsigned long avgPageTurnMs =
      gState.pageTurns == 0 ? 0 : gState.totalPageTurnMs / static_cast<unsigned long>(gState.pageTurns);
  unsigned long medianPageTurnMs = 0;
  if (gState.pageTurns > 0) {
    const std::size_t medianIndex = gState.pageTurns / 2;
    std::size_t seen = 0;
    for (std::size_t bucket = 0; bucket < gState.pageTurnHistogram.size(); ++bucket) {
      seen += gState.pageTurnHistogram[bucket];
      if (seen > medianIndex) {
        medianPageTurnMs = static_cast<unsigned long>(bucket == kMedianOverflowBucket ? kMedianHistogramMs : bucket);
        break;
      }
    }
  }
  const auto heapTotal = SimulatorHeap::totalBytes();

  std::printf(
      "SIM_BENCHMARK_RESULT epub=%s initial_load_ms=%lu page_turns=%zu total_page_turn_ms=%lu avg_page_turn_ms=%lu "
      "median_page_turn_ms=%lu max_page_turn_ms=%lu heap_total_bytes=%zu heap_free_bytes=%zu "
      "heap_min_free_bytes=%zu heap_max_alloc_bytes=%zu heap_fragmentation_pct=%zu heap_peak_used_bytes=%zu "
      "pointer_size=%zu\n",
      gState.epubPath.c_str(), gState.initialLoadMs, gState.pageTurns, gState.totalPageTurnMs, avgPageTurnMs,
      medianPageTurnMs, gState.maxPageTurnMs, heapTotal, SimulatorHeap::freeBytes(), SimulatorHeap::minFreeBytes(),
      SimulatorHeap::largestFreeBlockBytes(), SimulatorHeap::fragmentationPercent(), SimulatorHeap::peakUsedBytes(),
      sizeof(void*));
  std::fflush(stdout);
}

}  // namespace

namespace SimulatorBenchmark {

void initializeFromEnv() {
  if (gState.enabled) return;

  gState.enabled = parseBoolEnv("CROSSPOINT_SIM_BENCHMARK");
  if (!gState.enabled) return;

  const char* epubPath = std::getenv("CROSSPOINT_SIM_BENCH_EPUB");
  gState.epubPath = (epubPath && *epubPath) ? epubPath : "/books/benchmark.epub";
  gState.targetPages = parsePositiveIntEnv("CROSSPOINT_SIM_BENCH_PAGES", 100);
  gState.startSpine = parseNonNegativeIntEnv("CROSSPOINT_SIM_BENCH_START_SPINE", -1);
  gState.startPage = parseNonNegativeIntEnv("CROSSPOINT_SIM_BENCH_START_PAGE", 0);
  gState.clearCache = parseBoolEnv("CROSSPOINT_SIM_BENCH_CLEAR_CACHE");
}

bool isEnabled() { return gState.enabled; }

bool startIfConfigured() {
  initializeFromEnv();
  if (!gState.enabled || gState.started) return false;

  if (gState.clearCache) {
    Storage.rmdir("/.crosspoint");
  }

  LOG_INF("SIMBENCH", "Starting benchmark: epub=%s pages=%d", gState.epubPath.c_str(), gState.targetPages);
  gState.startMs = millis();

  activityManager.goToReader(gState.epubPath);
  activityManager.loop();
  activityManager.requestUpdateAndWait();

  EpubReaderActivity* reader = currentReader();
  if (!reader || !reader->simulatorHasLoadedPage()) {
    LOG_ERR("SIMBENCH", "Reader did not load benchmark epub: %s", gState.epubPath.c_str());
    gState.finished = true;
    return false;
  }

  if (gState.startSpine >= 0 &&
      (reader->simulatorCurrentSpineIndex() != gState.startSpine || reader->simulatorCurrentPageIndex() != gState.startPage)) {
    LOG_INF("SIMBENCH", "Jumping to start position: spine=%d page=%d", gState.startSpine, gState.startPage);
    reader->simulatorSetPosition(gState.startSpine, gState.startPage);
    activityManager.loop();
    activityManager.requestUpdateAndWait();
    reader = currentReader();
    if (!reader || !reader->simulatorHasLoadedPage()) {
      LOG_ERR("SIMBENCH", "Reader failed to jump to start position: spine=%d page=%d", gState.startSpine,
              gState.startPage);
      gState.finished = true;
      return false;
    }
  }

  gState.initialLoadMs = millis() - gState.startMs;
  gState.started = true;
  std::printf("SIM_BENCHMARK_LOAD spine=%d page=%d page_count=%d initial_load_ms=%lu\n",
              reader->simulatorCurrentSpineIndex(), reader->simulatorCurrentPageIndex(),
              reader->simulatorCurrentSectionPageCount(), gState.initialLoadMs);
  std::fflush(stdout);
  return true;
}

void tick() {
  if (!gState.started || gState.finished) return;

  EpubReaderActivity* reader = currentReader();
  if (!reader || !reader->simulatorHasLoadedPage()) {
    LOG_ERR("SIMBENCH", "Reader unavailable while benchmark was running");
    gState.finished = true;
    return;
  }

  if (reader->simulatorAtEndOfBook() || static_cast<int>(gState.pageTurns) >= gState.targetPages) {
    printSummary();
    gState.finished = true;
    std::_Exit(0);
  }

  const unsigned long start = millis();
  reader->simulatorPageTurnForward();
  activityManager.requestUpdateAndWait();
  const unsigned long elapsed = millis() - start;

  reader = currentReader();
  if (!reader || (!reader->simulatorHasLoadedPage() && !reader->simulatorAtEndOfBook())) {
    LOG_ERR("SIMBENCH", "Reader failed to render next page");
    gState.finished = true;
    std::_Exit(2);
  }

  gState.pageTurns++;
  gState.totalPageTurnMs += elapsed;
  gState.maxPageTurnMs = std::max(gState.maxPageTurnMs, elapsed);
  const std::size_t histogramBucket =
      std::min<std::size_t>(elapsed, kMedianOverflowBucket);
  gState.pageTurnHistogram[histogramBucket]++;
  std::printf(
      "SIM_BENCHMARK_PAGE index=%zu elapsed_ms=%lu spine=%d page=%d section_pages=%d heap_free_bytes=%zu "
      "heap_total_bytes=%zu heap_min_free_bytes=%zu heap_max_alloc_bytes=%zu heap_fragmentation_pct=%zu "
      "end_of_book=%d\n",
      gState.pageTurns, elapsed, reader ? reader->simulatorCurrentSpineIndex() : -1,
      reader ? reader->simulatorCurrentPageIndex() : -1, reader ? reader->simulatorCurrentSectionPageCount() : 0,
      SimulatorHeap::freeBytes(), SimulatorHeap::totalBytes(), SimulatorHeap::minFreeBytes(),
      SimulatorHeap::largestFreeBlockBytes(), SimulatorHeap::fragmentationPercent(),
      reader && reader->simulatorAtEndOfBook());
  std::fflush(stdout);

  if (reader && reader->simulatorAtEndOfBook()) {
    printSummary();
    gState.finished = true;
    std::_Exit(0);
  }
}

}  // namespace SimulatorBenchmark

#endif
