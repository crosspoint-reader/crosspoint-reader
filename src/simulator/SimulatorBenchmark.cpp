#ifdef SIMULATOR

#include "SimulatorBenchmark.h"

#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <string>
#include <vector>

#include "activities/ActivityManager.h"
#include "activities/reader/EpubReaderActivity.h"
#include "simulator/SimulatorHeap.h"

namespace {

struct BenchmarkState {
  bool enabled = false;
  bool clearCache = false;
  bool started = false;
  bool finished = false;
  std::string epubPath;
  int targetPages = 100;
  unsigned long initialLoadMs = 0;
  unsigned long startMs = 0;
  std::vector<unsigned long> pageTurnMs;
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

bool parseBoolEnv(const char* name) {
  const char* raw = std::getenv(name);
  if (!raw || !*raw) return false;
  return std::string(raw) != "0";
}

EpubReaderActivity* currentReader() {
  return dynamic_cast<EpubReaderActivity*>(activityManager.getCurrentActivityForSimulator());
}

void printSummary() {
  std::vector<unsigned long> sorted = gState.pageTurnMs;
  std::sort(sorted.begin(), sorted.end());

  const unsigned long totalPageTurnMs =
      std::accumulate(gState.pageTurnMs.begin(), gState.pageTurnMs.end(), 0UL);
  const unsigned long avgPageTurnMs =
      gState.pageTurnMs.empty() ? 0 : totalPageTurnMs / static_cast<unsigned long>(gState.pageTurnMs.size());
  const unsigned long medianPageTurnMs =
      sorted.empty() ? 0 : sorted[sorted.size() / 2];
  const unsigned long maxPageTurnMs = sorted.empty() ? 0 : sorted.back();
  const auto heapLimit = SimulatorHeap::heapLimitBytes();

  std::printf(
      "SIM_BENCHMARK_RESULT epub=%s initial_load_ms=%lu page_turns=%zu total_page_turn_ms=%lu avg_page_turn_ms=%lu "
      "median_page_turn_ms=%lu max_page_turn_ms=%lu heap_limit_bytes=%zu heap_peak_bytes=%zu heap_min_free_bytes=%zu "
      "pointer_size=%zu\n",
      gState.epubPath.c_str(), gState.initialLoadMs, gState.pageTurnMs.size(), totalPageTurnMs, avgPageTurnMs,
      medianPageTurnMs, maxPageTurnMs, heapLimit, SimulatorHeap::peakUsedBytes(), SimulatorHeap::minFreeBytes(),
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
  gState.pageTurnMs.reserve(static_cast<std::size_t>(gState.targetPages));

  activityManager.goToReader(gState.epubPath);
  activityManager.loop();
  activityManager.requestUpdateAndWait();

  EpubReaderActivity* reader = currentReader();
  if (!reader || !reader->simulatorHasLoadedPage()) {
    LOG_ERR("SIMBENCH", "Reader did not load benchmark epub: %s", gState.epubPath.c_str());
    gState.finished = true;
    return false;
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

  if (reader->simulatorAtEndOfBook() || static_cast<int>(gState.pageTurnMs.size()) >= gState.targetPages) {
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

  gState.pageTurnMs.push_back(elapsed);
  std::printf(
      "SIM_BENCHMARK_PAGE index=%zu elapsed_ms=%lu spine=%d page=%d section_pages=%d heap_used_bytes=%zu "
      "heap_free_bytes=%zu end_of_book=%d\n",
      gState.pageTurnMs.size(), elapsed, reader ? reader->simulatorCurrentSpineIndex() : -1,
      reader ? reader->simulatorCurrentPageIndex() : -1, reader ? reader->simulatorCurrentSectionPageCount() : 0,
      SimulatorHeap::currentUsedBytes(), SimulatorHeap::freeBytes(), reader && reader->simulatorAtEndOfBook());
  std::fflush(stdout);

  if (reader && reader->simulatorAtEndOfBook()) {
    printSummary();
    gState.finished = true;
    std::_Exit(0);
  }
}

}  // namespace SimulatorBenchmark

#endif
