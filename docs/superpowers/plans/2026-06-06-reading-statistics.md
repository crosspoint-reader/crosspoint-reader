# Reading Statistics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Track per-book and all-time reading time, pages read, and reading speed (pages/hour) on CrossPoint, persisted to the SD card.

**Architecture:** A pure-logic aggregation engine (`lib/ReadingStats`, no Arduino deps) accumulates reading time from monotonic `millis()` deltas with a per-page idle cap, so it behaves identically on X3 and X4 (neither device has a reliable calendar clock). A thin singleton store (`src/ReadingStatsStore`) persists the engine's data to `/.crosspoint/reading_stats.json` via `JsonSettingsIO`, mirroring `RecentBooksStore`. `EpubReaderActivity` drives the engine from its session lifecycle: `onEnter` → `beginSession`, `pageTurn` → `recordPageTurn`, `onExit` → `endSession`.

**Tech Stack:** C++20, PlatformIO (ESP32-C3 / Arduino), GoogleTest host-side unit tests (CMake/CTest), ArduinoJson 7.

**Scope (v1):** time + pages + speed, per-book and global. **Out of scope (no reliable date source on hardware):** daily streaks, pages/day, calendar history. **Out of scope (separate follow-up plan):** the on-device Statistics screen + menu entry. In v1 the data is inspectable via the JSON file over the existing file-transfer web UI.

**Commit convention for this project:** plain commit messages, **no `Co-Authored-By` trailer**, no Claude attribution.

---

## File Structure

| File | Responsibility | Tested |
|------|----------------|--------|
| `lib/ReadingStats/ReadingStats.h` | `BookStats` struct + `ReadingStatsAggregator` declarations (pure logic) | host gtest |
| `lib/ReadingStats/ReadingStats.cpp` | aggregation logic: session timing, idle cap, counters, speed | host gtest |
| `test/reading_stats/ReadingStatsTest.cpp` | gtest suite for the aggregator | — |
| `test/reading_stats/CMakeLists.txt` | builds the suite | — |
| `test/CMakeLists.txt` | register `reading_stats` subdir (modify) | — |
| `src/ReadingStatsStore.h` / `.cpp` | singleton wrapper + SD persistence (mirrors `RecentBooksStore`) | manual |
| `src/JsonSettingsIO.h` / `.cpp` | `saveReadingStats` / `loadReadingStats` (mirrors recent-books JSON) | manual |
| `src/activities/reader/EpubReaderActivity.cpp` | wire session lifecycle to the store | manual |
| `src/main.cpp` | load stats at boot | manual |

The engine lives in `lib/` (like `lib/JsonParser`) because the host test harness includes `${REPO_ROOT}/lib`. Anything touching `Storage`/`ArduinoJson`/`Epub` stays in `src/` and is verified by building the firmware + on-device, exactly like `RecentBooksStore`.

---

## Task 1: Engine skeleton + test harness wiring + empty-state test

**Files:**
- Create: `lib/ReadingStats/ReadingStats.h`
- Create: `lib/ReadingStats/ReadingStats.cpp`
- Create: `test/reading_stats/ReadingStatsTest.cpp`
- Create: `test/reading_stats/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write the header**

Create `lib/ReadingStats/ReadingStats.h`:

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace reading_stats {

// Per-book accumulated reading statistics. Time is wall-clock-free: it is
// derived from monotonic millis() deltas, so it works identically on X3 and
// X4 (neither exposes a reliable calendar date).
struct BookStats {
  std::string bookPath;         // identity key (epub->getPath())
  uint32_t pagesRead = 0;       // forward page turns only
  uint32_t totalReadingMs = 0;  // summed, with each gap capped (see kMaxPageMs)
  uint32_t sessionCount = 0;    // number of ended reading sessions

  bool operator==(const BookStats& o) const {
    return bookPath == o.bookPath && pagesRead == o.pagesRead && totalReadingMs == o.totalReadingMs &&
           sessionCount == o.sessionCount;
  }
};

// Pure aggregation engine. No Arduino/SD dependencies, so it is unit-tested on
// the host. Persistence lives in ReadingStatsStore (src/), mirroring
// RecentBooksStore. All timestamps are millis() values (monotonic, ms).
class ReadingStatsAggregator {
 public:
  // The time attributed to a single page turn is capped at this value, so a
  // device left on an open page does not inflate reading time. A gap longer
  // than this still counts, but only up to the cap.
  static constexpr uint32_t kMaxPageMs = 300000;  // 5 minutes

  // Replace all per-book stats with previously persisted data.
  void load(std::vector<BookStats> books);

  // Snapshot for persistence.
  const std::vector<BookStats>& books() const { return books_; }

  // Begin a reading session for a book. If a session is already active it is
  // ended first (banked at nowMs) before the new one starts.
  void beginSession(const std::string& bookPath, uint32_t nowMs);

  // Record a page turn within the active session. `forward` true counts the
  // page as read; either direction banks elapsed time. No-op without a session.
  void recordPageTurn(uint32_t nowMs, bool forward);

  // End the active session, banking the final delta and counting the session.
  // No-op if no session is active.
  void endSession(uint32_t nowMs);

  // Aggregate accessors.
  const BookStats* statsFor(const std::string& bookPath) const;
  uint32_t totalPagesRead() const;
  uint32_t totalReadingMs() const;

  // Reading speed for one book in pages per hour (0 if no time recorded).
  uint32_t pagesPerHour(const std::string& bookPath) const;

 private:
  // Wrap-safe, capped delta from lastEventMs_ to nowMs.
  uint32_t cappedDelta(uint32_t nowMs) const;

  std::vector<BookStats> books_;
  bool sessionActive_ = false;
  int activeIndex_ = -1;  // index into books_ for the active session
  uint32_t lastEventMs_ = 0;
};

}  // namespace reading_stats
```

- [ ] **Step 2: Write the implementation (begin/record/end are stubs for now)**

Create `lib/ReadingStats/ReadingStats.cpp`:

```cpp
#include "ReadingStats.h"

#include <algorithm>

namespace reading_stats {

void ReadingStatsAggregator::load(std::vector<BookStats> books) {
  books_ = std::move(books);
  sessionActive_ = false;
  activeIndex_ = -1;
  lastEventMs_ = 0;
}

uint32_t ReadingStatsAggregator::cappedDelta(uint32_t nowMs) const {
  // millis() wraps roughly every 49.7 days; treat a backwards jump as no time.
  if (nowMs < lastEventMs_) return 0;
  const uint32_t delta = nowMs - lastEventMs_;
  return std::min(delta, kMaxPageMs);
}

void ReadingStatsAggregator::beginSession(const std::string&, uint32_t) {
  // Implemented in Task 2.
}

void ReadingStatsAggregator::recordPageTurn(uint32_t, bool) {
  // Implemented in Task 2.
}

void ReadingStatsAggregator::endSession(uint32_t) {
  // Implemented in Task 2.
}

const BookStats* ReadingStatsAggregator::statsFor(const std::string& bookPath) const {
  auto it = std::find_if(books_.begin(), books_.end(),
                         [&](const BookStats& b) { return b.bookPath == bookPath; });
  return it == books_.end() ? nullptr : &*it;
}

uint32_t ReadingStatsAggregator::totalPagesRead() const {
  uint32_t total = 0;
  for (const auto& b : books_) total += b.pagesRead;
  return total;
}

uint32_t ReadingStatsAggregator::totalReadingMs() const {
  uint32_t total = 0;
  for (const auto& b : books_) total += b.totalReadingMs;
  return total;
}

uint32_t ReadingStatsAggregator::pagesPerHour(const std::string& bookPath) const {
  const BookStats* s = statsFor(bookPath);
  if (!s || s->totalReadingMs == 0) return 0;
  return static_cast<uint32_t>(static_cast<uint64_t>(s->pagesRead) * 3600000ULL / s->totalReadingMs);
}

}  // namespace reading_stats
```

- [ ] **Step 3: Write the empty-state test**

Create `test/reading_stats/ReadingStatsTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include "ReadingStats.h"

using reading_stats::BookStats;
using reading_stats::ReadingStatsAggregator;

TEST(ReadingStats, EmptyAggregatorHasNoData) {
  ReadingStatsAggregator agg;
  EXPECT_EQ(agg.totalPagesRead(), 0u);
  EXPECT_EQ(agg.totalReadingMs(), 0u);
  EXPECT_EQ(agg.statsFor("/books/missing.epub"), nullptr);
  EXPECT_EQ(agg.pagesPerHour("/books/missing.epub"), 0u);
  EXPECT_TRUE(agg.books().empty());
}
```

- [ ] **Step 4: Write the test CMakeLists**

Create `test/reading_stats/CMakeLists.txt`:

```cmake
add_executable(ReadingStatsTest
  ReadingStatsTest.cpp
  ${REPO_ROOT}/lib/ReadingStats/ReadingStats.cpp
)

target_include_directories(ReadingStatsTest PRIVATE
  ${REPO_ROOT}/lib/ReadingStats
)

target_link_libraries(ReadingStatsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingStatsTest)
```

- [ ] **Step 5: Register the suite in the root test CMakeLists**

In `test/CMakeLists.txt`, find the block of `add_subdirectory(...)` lines at the end:

```cmake
add_subdirectory(streaming_json_parser)
add_subdirectory(release_json_parser)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
```

Change it to add the new suite:

```cmake
add_subdirectory(streaming_json_parser)
add_subdirectory(release_json_parser)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(reading_stats)
```

- [ ] **Step 6: Configure and run the suite**

Run:
```bash
cmake -S test -B build/test -DCMAKE_BUILD_TYPE=Release
cmake --build build/test --target ReadingStatsTest
./build/test/reading_stats/ReadingStatsTest --gtest_filter='ReadingStats.EmptyAggregatorHasNoData'
```
Expected: builds clean, `[  PASSED  ] 1 test.`

- [ ] **Step 7: Commit**

```bash
git add lib/ReadingStats test/reading_stats test/CMakeLists.txt
git commit -m "Add ReadingStats aggregator skeleton with host test harness"
```

---

## Task 2: Session timing and page counting

**Files:**
- Modify: `lib/ReadingStats/ReadingStats.cpp` (replace the three stubbed methods)
- Test: `test/reading_stats/ReadingStatsTest.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `test/reading_stats/ReadingStatsTest.cpp`:

```cpp
TEST(ReadingStats, BanksTimeAndCountsForwardPages) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/a.epub", 1000);
  agg.recordPageTurn(3000, true);   // +2000 ms, +1 page
  agg.recordPageTurn(8000, true);   // +5000 ms, +1 page
  agg.endSession(9000);             // +1000 ms

  const BookStats* s = agg.statsFor("/books/a.epub");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->pagesRead, 2u);
  EXPECT_EQ(s->totalReadingMs, 8000u);  // 2000 + 5000 + 1000
  EXPECT_EQ(s->sessionCount, 1u);
}

TEST(ReadingStats, BackwardTurnBanksTimeButNotPages) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/a.epub", 0);
  agg.recordPageTurn(1000, true);    // +1000 ms, +1 page
  agg.recordPageTurn(2000, false);   // +1000 ms, no page
  agg.endSession(2000);              // +0 ms

  const BookStats* s = agg.statsFor("/books/a.epub");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->pagesRead, 1u);
  EXPECT_EQ(s->totalReadingMs, 2000u);
}

TEST(ReadingStats, PageTurnWithoutSessionIsIgnored) {
  ReadingStatsAggregator agg;
  agg.recordPageTurn(1000, true);
  agg.endSession(2000);
  EXPECT_EQ(agg.totalPagesRead(), 0u);
  EXPECT_EQ(agg.totalReadingMs(), 0u);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run:
```bash
cmake --build build/test --target ReadingStatsTest && \
./build/test/reading_stats/ReadingStatsTest --gtest_filter='ReadingStats.BanksTimeAndCountsForwardPages:ReadingStats.BackwardTurnBanksTimeButNotPages:ReadingStats.PageTurnWithoutSessionIsIgnored'
```
Expected: FAIL — `pagesRead` is 0, `statsFor` returns nullptr (the stubs do nothing).

- [ ] **Step 3: Implement the three methods**

In `lib/ReadingStats/ReadingStats.cpp`, replace the three stub bodies:

```cpp
void ReadingStatsAggregator::beginSession(const std::string& bookPath, uint32_t nowMs) {
  if (sessionActive_) endSession(nowMs);

  auto it = std::find_if(books_.begin(), books_.end(),
                         [&](const BookStats& b) { return b.bookPath == bookPath; });
  if (it == books_.end()) {
    BookStats fresh;
    fresh.bookPath = bookPath;
    books_.push_back(std::move(fresh));
    activeIndex_ = books_.size() - 1;
  } else {
    activeIndex_ = static_cast<std::size_t>(std::distance(books_.begin(), it));
  }
  sessionActive_ = true;
  lastEventMs_ = nowMs;
}

void ReadingStatsAggregator::recordPageTurn(uint32_t nowMs, bool forward) {
  if (!sessionActive_) return;
  BookStats& book = books_[*activeIndex_];
  book.totalReadingMs += cappedDelta(nowMs);
  if (forward) book.pagesRead++;
  lastEventMs_ = nowMs;
}

void ReadingStatsAggregator::endSession(uint32_t nowMs) {
  if (!sessionActive_) return;
  BookStats& book = books_[*activeIndex_];
  book.totalReadingMs += cappedDelta(nowMs);
  book.sessionCount++;
  sessionActive_ = false;
  activeIndex_.reset();
}
```

> Note: `activeIndex_` is a `std::optional<std::size_t>` (Task 1 review fix); `*activeIndex_` dereferences the active session's index. `sessionActive_` guards every dereference, so the optional always holds a value when used here.

- [ ] **Step 4: Run tests to verify they pass**

Run:
```bash
cmake --build build/test --target ReadingStatsTest && \
./build/test/reading_stats/ReadingStatsTest --gtest_filter='ReadingStats.*'
```
Expected: PASS — all 4 tests green.

- [ ] **Step 5: Commit**

```bash
git add lib/ReadingStats/ReadingStats.cpp test/reading_stats/ReadingStatsTest.cpp
git commit -m "Implement ReadingStats session timing and page counting"
```

---

## Task 3: Idle cap and millis() wrap safety

**Files:**
- Test: `test/reading_stats/ReadingStatsTest.cpp`

The behaviour is already implemented in `cappedDelta` (Task 1). These tests pin it down so a future change can't silently break it.

- [ ] **Step 1: Write the tests**

Append to `test/reading_stats/ReadingStatsTest.cpp`:

```cpp
TEST(ReadingStats, LongGapIsCappedAtMaxPageMs) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/a.epub", 0);
  // Gap of 1 hour on one page: only kMaxPageMs (5 min) should be counted.
  agg.recordPageTurn(3600000, true);
  agg.endSession(3600000);

  const BookStats* s = agg.statsFor("/books/a.epub");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->totalReadingMs, ReadingStatsAggregator::kMaxPageMs);
}

TEST(ReadingStats, MillisWrapCountsAsZeroDelta) {
  ReadingStatsAggregator agg;
  // Start near the uint32 millis() ceiling, then wrap past zero.
  agg.beginSession("/books/a.epub", 0xFFFFFF00u);
  agg.recordPageTurn(0x00000100u, true);  // nowMs < lastEventMs_ -> 0 ms
  agg.endSession(0x00000100u);

  const BookStats* s = agg.statsFor("/books/a.epub");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->pagesRead, 1u);
  EXPECT_EQ(s->totalReadingMs, 0u);
}
```

- [ ] **Step 2: Run tests to verify they pass**

Run:
```bash
cmake --build build/test --target ReadingStatsTest && \
./build/test/reading_stats/ReadingStatsTest --gtest_filter='ReadingStats.LongGapIsCappedAtMaxPageMs:ReadingStats.MillisWrapCountsAsZeroDelta'
```
Expected: PASS — both green (logic already present, so no implementation change).

- [ ] **Step 3: Commit**

```bash
git add test/reading_stats/ReadingStatsTest.cpp
git commit -m "Test ReadingStats idle cap and millis wrap handling"
```

---

## Task 4: Multiple books, session switching, and session counting

**Files:**
- Test: `test/reading_stats/ReadingStatsTest.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `test/reading_stats/ReadingStatsTest.cpp`:

```cpp
TEST(ReadingStats, TracksTwoBooksIndependently) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/a.epub", 0);
  agg.recordPageTurn(1000, true);
  agg.endSession(1000);

  agg.beginSession("/books/b.epub", 5000);
  agg.recordPageTurn(7000, true);
  agg.recordPageTurn(9000, true);
  agg.endSession(9000);

  EXPECT_EQ(agg.statsFor("/books/a.epub")->pagesRead, 1u);
  EXPECT_EQ(agg.statsFor("/books/b.epub")->pagesRead, 2u);
  EXPECT_EQ(agg.totalPagesRead(), 3u);
}

TEST(ReadingStats, BeginSessionEndsThePriorSession) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/a.epub", 0);
  agg.recordPageTurn(1000, true);     // book a: +1000 ms, +1 page
  // Switch to b without an explicit endSession; a is banked at nowMs (2000).
  agg.beginSession("/books/b.epub", 2000);
  agg.recordPageTurn(3000, true);     // book b: +1000 ms, +1 page
  agg.endSession(3000);

  const BookStats* a = agg.statsFor("/books/a.epub");
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->totalReadingMs, 2000u);  // 1000 (to first turn) + 1000 (banked on switch)
  EXPECT_EQ(a->sessionCount, 1u);       // switching ended a's session

  const BookStats* b = agg.statsFor("/books/b.epub");
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->totalReadingMs, 1000u);
  EXPECT_EQ(b->sessionCount, 1u);
}

TEST(ReadingStats, ReopeningSameBookAccumulates) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/a.epub", 0);
  agg.recordPageTurn(1000, true);
  agg.endSession(1000);

  agg.beginSession("/books/a.epub", 2000);
  agg.recordPageTurn(3000, true);
  agg.endSession(3000);

  const BookStats* s = agg.statsFor("/books/a.epub");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->pagesRead, 2u);
  EXPECT_EQ(s->totalReadingMs, 2000u);
  EXPECT_EQ(s->sessionCount, 2u);
  EXPECT_EQ(agg.books().size(), 1u);  // not duplicated
}
```

- [ ] **Step 2: Run tests**

Run:
```bash
cmake --build build/test --target ReadingStatsTest && \
./build/test/reading_stats/ReadingStatsTest --gtest_filter='ReadingStats.TracksTwoBooksIndependently:ReadingStats.BeginSessionEndsThePriorSession:ReadingStats.ReopeningSameBookAccumulates'
```
Expected: PASS — the Task 2 implementation already supports this. (If any fail, fix `beginSession`/`endSession` in `lib/ReadingStats/ReadingStats.cpp` until green; do not change the tests.)

- [ ] **Step 3: Commit**

```bash
git add test/reading_stats/ReadingStatsTest.cpp
git commit -m "Test ReadingStats multi-book tracking and session switching"
```

---

## Task 5: Reading speed and load() roundtrip

**Files:**
- Test: `test/reading_stats/ReadingStatsTest.cpp`

- [ ] **Step 1: Write the tests**

Append to `test/reading_stats/ReadingStatsTest.cpp`:

```cpp
TEST(ReadingStats, PagesPerHourComputesSpeed) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/a.epub", 0);
  // 30 pages over exactly 1 hour -> 30 pages/hour. Use forward turns and a
  // final endSession that adds no extra time (each gap <= kMaxPageMs).
  uint32_t t = 0;
  for (int i = 0; i < 30; ++i) {
    t += 120000;  // 2 min per page (under the 5 min cap)
    agg.recordPageTurn(t, true);
  }
  agg.endSession(t);

  const BookStats* s = agg.statsFor("/books/a.epub");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->pagesRead, 30u);
  EXPECT_EQ(s->totalReadingMs, 3600000u);  // 30 * 120000
  EXPECT_EQ(agg.pagesPerHour("/books/a.epub"), 30u);
}

TEST(ReadingStats, PagesPerHourIsZeroWithoutTime) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/a.epub", 1000);
  agg.recordPageTurn(1000, true);  // zero elapsed
  agg.endSession(1000);
  EXPECT_EQ(agg.pagesPerHour("/books/a.epub"), 0u);
}

TEST(ReadingStats, LoadReplacesContentsAndResetsSession) {
  ReadingStatsAggregator agg;
  agg.beginSession("/books/stale.epub", 0);  // active session that load() must drop

  std::vector<BookStats> persisted;
  persisted.push_back(BookStats{"/books/a.epub", 10, 600000, 3});
  persisted.push_back(BookStats{"/books/b.epub", 5, 300000, 1});
  agg.load(persisted);

  EXPECT_EQ(agg.books().size(), 2u);
  EXPECT_EQ(agg.totalPagesRead(), 15u);
  EXPECT_EQ(agg.statsFor("/books/a.epub")->sessionCount, 3u);
  EXPECT_EQ(agg.statsFor("/books/stale.epub"), nullptr);

  // After load the prior session is gone, so a stray page turn is ignored
  // until a new beginSession.
  agg.recordPageTurn(1000, true);
  EXPECT_EQ(agg.totalPagesRead(), 15u);
}
```

- [ ] **Step 2: Run tests**

Run:
```bash
cmake --build build/test --target ReadingStatsTest && \
./build/test/reading_stats/ReadingStatsTest --gtest_filter='ReadingStats.PagesPerHourComputesSpeed:ReadingStats.PagesPerHourIsZeroWithoutTime:ReadingStats.LoadReplacesContentsAndResetsSession'
```
Expected: PASS — all green (speed + load already implemented in Task 1).

- [ ] **Step 3: Run the full suite via PlatformIO too (parity with CI)**

Run:
```bash
pio run -t unit-tests
```
Expected: CMake/CTest builds every suite and `ReadingStatsTest` reports all tests passed.

- [ ] **Step 4: Commit**

```bash
git add test/reading_stats/ReadingStatsTest.cpp
git commit -m "Test ReadingStats reading-speed and load roundtrip"
```

---

## Task 6: ReadingStatsStore singleton + SD persistence

This mirrors `src/RecentBooksStore.{h,cpp}` and is verified by building the firmware (it depends on `HalStorage`/`ArduinoJson`, which are not available to the host test harness — exactly like `RecentBooksStore`).

**Files:**
- Create: `src/ReadingStatsStore.h`
- Create: `src/ReadingStatsStore.cpp`
- Modify: `src/JsonSettingsIO.h`
- Modify: `src/JsonSettingsIO.cpp`

- [ ] **Step 1: Write the store header**

Create `src/ReadingStatsStore.h`:

```cpp
#pragma once
#include <ReadingStats.h>

#include <string>
#include <vector>

// Singleton wrapper around the pure ReadingStatsAggregator that adds SD-card
// persistence. Mirrors RecentBooksStore. Reading data is stored at
// /.crosspoint/reading_stats.json.
class ReadingStatsStore {
  static ReadingStatsStore instance;
  reading_stats::ReadingStatsAggregator aggregator;

 public:
  static ReadingStatsStore& getInstance() { return instance; }

  // --- Session lifecycle (called from the reader) ---
  void beginSession(const std::string& bookPath, uint32_t nowMs) { aggregator.beginSession(bookPath, nowMs); }
  void recordPageTurn(uint32_t nowMs, bool forward) { aggregator.recordPageTurn(nowMs, forward); }
  // Ends the active session and persists. Best-effort: a failed save is logged.
  void endSession(uint32_t nowMs);

  // --- Persistence ---
  bool saveToFile() const;
  bool loadFromFile();

  // --- Accessors used by JsonSettingsIO and (later) the stats screen ---
  const std::vector<reading_stats::BookStats>& books() const { return aggregator.books(); }
  void loadBooks(std::vector<reading_stats::BookStats> books) { aggregator.load(std::move(books)); }
  const reading_stats::BookStats* statsFor(const std::string& path) const { return aggregator.statsFor(path); }
  uint32_t totalPagesRead() const { return aggregator.totalPagesRead(); }
  uint32_t totalReadingMs() const { return aggregator.totalReadingMs(); }
  uint32_t pagesPerHour(const std::string& path) const { return aggregator.pagesPerHour(path); }
};

// Helper macro mirroring RECENT_BOOKS.
#define READING_STATS ReadingStatsStore::getInstance()
```

> Note: `#include <ReadingStats.h>` resolves because `lib/ReadingStats` is a PlatformIO library directory, so its headers are on the include path for firmware builds (same mechanism as `Epub.h`, `HalStorage.h`).

- [ ] **Step 2: Write the store implementation**

Create `src/ReadingStatsStore.cpp`:

```cpp
#include "ReadingStatsStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include "JsonSettingsIO.h"

namespace {
constexpr char READING_STATS_FILE_JSON[] = "/.crosspoint/reading_stats.json";
}  // namespace

ReadingStatsStore ReadingStatsStore::instance;

void ReadingStatsStore::endSession(uint32_t nowMs) {
  aggregator.endSession(nowMs);
  if (!saveToFile()) {
    LOG_ERR("STATS", "Failed to persist reading stats");
  }
}

bool ReadingStatsStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveReadingStats(*this, READING_STATS_FILE_JSON);
}

bool ReadingStatsStore::loadFromFile() {
  if (Storage.exists(READING_STATS_FILE_JSON)) {
    String json = Storage.readFile(READING_STATS_FILE_JSON);
    if (!json.isEmpty()) {
      return JsonSettingsIO::loadReadingStats(*this, json.c_str());
    }
  }
  return false;
}
```

- [ ] **Step 3: Declare the JSON helpers**

In `src/JsonSettingsIO.h`, add the forward declaration next to the other store forward declarations (after `class OpdsServerStore;`):

```cpp
class ReadingStatsStore;
```

And add the function declarations inside `namespace JsonSettingsIO {`, after the `RecentBooksStore` block:

```cpp
// ReadingStatsStore
bool saveReadingStats(const ReadingStatsStore& store, const char* path);
bool loadReadingStats(ReadingStatsStore& store, const char* json);
```

- [ ] **Step 4: Implement the JSON helpers**

In `src/JsonSettingsIO.cpp`, add the include near the other store includes (after `#include "RecentBooksStore.h"`):

```cpp
#include "ReadingStatsStore.h"
```

Add this section after the `// ---- RecentBooksStore ----` block (before `// ---- OpdsServerStore ----`):

```cpp
// ---- ReadingStatsStore ----

bool JsonSettingsIO::saveReadingStats(const ReadingStatsStore& store, const char* path) {
  JsonDocument doc;
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : store.books()) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.bookPath;
    obj["pagesRead"] = book.pagesRead;
    obj["totalReadingMs"] = book.totalReadingMs;
    obj["sessionCount"] = book.sessionCount;
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadReadingStats(ReadingStatsStore& store, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("STATS", "JSON parse error: %s", error.c_str());
    return false;
  }

  std::vector<reading_stats::BookStats> books;
  JsonArray arr = doc["books"].as<JsonArray>();
  for (JsonObject obj : arr) {
    reading_stats::BookStats book;
    book.bookPath = obj["path"] | std::string("");
    book.pagesRead = obj["pagesRead"] | 0u;
    book.totalReadingMs = obj["totalReadingMs"] | 0u;
    book.sessionCount = obj["sessionCount"] | 0u;
    if (!book.bookPath.empty()) books.push_back(std::move(book));
  }

  store.loadBooks(std::move(books));
  LOG_DBG("STATS", "Reading stats loaded (%d books)", static_cast<int>(store.books().size()));
  return true;
}
```

- [ ] **Step 5: Build the firmware to verify it compiles**

Run:
```bash
pio run -e default
```
Expected: build succeeds (`SUCCESS`). No new warnings from the added files.

- [ ] **Step 6: Commit**

```bash
git add src/ReadingStatsStore.h src/ReadingStatsStore.cpp src/JsonSettingsIO.h src/JsonSettingsIO.cpp
git commit -m "Add ReadingStatsStore with SD-card JSON persistence"
```

---

## Task 7: Load stats at boot

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Add the include**

In `src/main.cpp`, add the store include alongside the other store includes. Find an existing line such as:

```cpp
#include "RecentBooksStore.h"
```

(If `main.cpp` includes stores indirectly, add the include near the top of the `#include "..."` project headers.) Add directly after it:

```cpp
#include "ReadingStatsStore.h"
```

- [ ] **Step 2: Load at boot**

In `src/main.cpp`, the boot sequence loads each store around lines 347-352:

```cpp
  SETTINGS.loadFromFile();
  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();

  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
```

Add the stats load immediately after `OPDS_STORE.loadFromFile();`:

```cpp
  OPDS_STORE.loadFromFile();
  READING_STATS.loadFromFile();
```

- [ ] **Step 3: Build to verify**

Run:
```bash
pio run -e default
```
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "Load reading stats at boot"
```

---

## Task 8: Wire the reader session lifecycle

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.cpp`

- [ ] **Step 1: Add the include**

In `src/activities/reader/EpubReaderActivity.cpp`, add the store include near the existing includes (the file already includes project headers). Add:

```cpp
#include "ReadingStatsStore.h"
```

- [ ] **Step 2: Begin the session in onEnter**

In `EpubReaderActivity::onEnter()`, find the recent-books add near line 166:

```cpp
  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
```

Add the session start immediately after the `RECENT_BOOKS.addBook(...)` line:

```cpp
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
  READING_STATS.beginSession(epub->getPath(), millis());
```

- [ ] **Step 3: Record page turns**

In `EpubReaderActivity::pageTurn(bool isForwardTurn)`, the method ends with:

```cpp
  lastPageTurnTime = millis();
  requestUpdate();
}
```

Insert the page-turn record so it reuses the same timestamp source:

```cpp
  lastPageTurnTime = millis();
  READING_STATS.recordPageTurn(lastPageTurnTime, isForwardTurn);
  requestUpdate();
}
```

- [ ] **Step 4: End the session in onExit**

In `EpubReaderActivity::onExit()`, end the session at the very top of the body, before `epub` is reset or moved. The current method starts:

```cpp
void EpubReaderActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
```

Add the end-session call right after `Activity::onExit();`:

```cpp
void EpubReaderActivity::onExit() {
  Activity::onExit();

  // Bank and persist this reading session before any teardown.
  READING_STATS.endSession(millis());

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
```

> `endSession` does not need the book path — the active session already knows it. It is placed before `epub.reset()` only so the save happens while the reader state is still coherent.

- [ ] **Step 5: Build to verify**

Run:
```bash
pio run -e default
```
Expected: build succeeds.

- [ ] **Step 6: Commit**

```bash
git add src/activities/reader/EpubReaderActivity.cpp
git commit -m "Track reading sessions from the EPUB reader"
```

---

## Task 9: Pre-PR checks and on-device verification

**Files:** none (verification only)

- [ ] **Step 1: Format and static-analysis (project pre-PR checks)**

Run:
```bash
./bin/clang-format-fix
pio check -e default
pio run -e default
pio run -t unit-tests
```
Expected: clang-format reports no changes needed (or applies them — if it does, re-commit), `pio check` reports no new defects in the added files, firmware builds, all unit tests pass.

- [ ] **Step 2: On-device smoke test**

Flash and exercise the feature:
```bash
pio run --target upload
```
Then on the device:
1. Open a book, turn several pages forward, turn one back, exit to Home.
2. Re-open the same book, turn a few more pages, exit.
3. Open a *second* book, turn pages, exit.

- [ ] **Step 3: Verify the persisted file**

Over the file-transfer web UI (or by reading the SD card directly), open `/.crosspoint/reading_stats.json`. Expected contents shape:

```json
{"books":[
  {"path":"/books/first.epub","pagesRead":7,"totalReadingMs":42000,"sessionCount":2},
  {"path":"/books/second.epub","pagesRead":3,"totalReadingMs":18000,"sessionCount":1}
]}
```
Confirm: `pagesRead` counts only forward turns; `sessionCount` increments once per open/close; `totalReadingMs` is plausible (idle time does not balloon it — leave a book open >5 min and confirm that page contributes at most `kMaxPageMs`).

- [ ] **Step 4: Commit any formatting fixups**

```bash
git add -A
git commit -m "Apply clang-format fixups for reading stats"
```
(Skip if Step 1 produced no changes.)

---

## Self-Review notes

- **Spec coverage:** time (`totalReadingMs`) ✓, pages (`pagesRead`) ✓, speed (`pagesPerHour`) ✓, per-book ✓, global (`totalPagesRead`/`totalReadingMs`) ✓, persistence ✓, reader wiring ✓, boot load ✓. Daily streaks / pages-per-day intentionally excluded (no reliable date source: DS3231 is X3-only and `HalClock` exposes no date; X4 has no RTC).
- **Type consistency:** `BookStats` fields (`bookPath`, `pagesRead`, `totalReadingMs`, `sessionCount`) and method names (`beginSession`, `recordPageTurn`, `endSession`, `load`/`loadBooks`, `books`, `statsFor`, `pagesPerHour`) are used identically across the engine, store, JSON helpers, and reader.
- **No placeholders:** every step contains the full code or exact command.
- **Future work (separate plan):** on-device Statistics screen + menu entry (touches `HomeMenuItem` enum, `HomeActivity` index mapping, `ActivityManager::goTo*`, i18n `STR_*` strings, e-ink rendering); optional NTP-seeded date layer to enable streaks.
