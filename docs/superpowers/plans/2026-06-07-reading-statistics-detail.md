# Reading Statistics — Book Detail Page (Plan 3)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use `- [ ]`.

**Goal:** Selecting a book in the Reading Statistics list opens a **detail page** showing that book's metrics in full, plus a **comparative bar** of its reading speed vs. the all-books average.

**Architecture:** Builds on Plan 2. Adds pure compute helpers (host-tested) for the derived metrics; a new `ReadingStatsDetailActivity` (pushed from the list via `startActivityForResult`, `finish()` returns to the list); i18n labels; and a Confirm handler in `ReadingStatsActivity`. No data-model change — everything is derived from the existing `BookStats` aggregates + global totals. (Per-session/time-series graphs are out of scope: no stored history, no reliable RTC.)

**Tech Stack:** C++20, PlatformIO ESP32-C3, GoogleTest (host), e-ink GUI (`renderer.drawText/fillRect/drawRect/getTextWidth`, `GUI.drawHeader/drawButtonHints`).

**Build/test:** firmware `~/.pio312/bin/pio run -e default`; host `cmake --build build/test --target ReadingStatsTest && ./build/test/reading_stats/ReadingStatsTest --gtest_filter='*'`; format `clang-format -i --style=file <files>`.

**Commits:** plain messages, no `Co-Authored-By`, no AI attribution.

---

## File Structure

| File | Responsibility | Tested |
|------|----------------|--------|
| `lib/ReadingStats/StatsFormat.h` / `.cpp` | add `pagesPerHour`, `avgMsPerPage`, `avgMsPerSession` (modify) | host gtest |
| `test/reading_stats/ReadingStatsTest.cpp` | add cases (modify) | — |
| `lib/I18n/translations/english.yaml` | detail labels (modify) | — |
| `src/activities/home/ReadingStatsDetailActivity.h` / `.cpp` | the detail page | manual |
| `src/activities/home/ReadingStatsActivity.{h,cpp}` | Confirm → open detail (modify) | manual |

---

## Task 1: Pure compute helpers (TDD)

**Files:** modify `lib/ReadingStats/StatsFormat.h`, `lib/ReadingStats/StatsFormat.cpp`, `test/reading_stats/ReadingStatsTest.cpp`

- [ ] **Step 1: Declare** — add to `lib/ReadingStats/StatsFormat.h` inside `namespace reading_stats {` (after `pathToDisplayName`):

```cpp
// Reading speed in pages per hour (0 if no time). uint64 intermediate to avoid overflow.
uint32_t pagesPerHour(uint32_t pages, uint32_t totalMs);

// Average milliseconds spent per page (0 if no pages).
uint32_t avgMsPerPage(uint32_t totalMs, uint32_t pages);

// Average milliseconds per reading session (0 if no sessions).
uint32_t avgMsPerSession(uint32_t totalMs, uint32_t sessions);
```

- [ ] **Step 2: Implement** — add to `lib/ReadingStats/StatsFormat.cpp` inside `namespace reading_stats {`:

```cpp
uint32_t pagesPerHour(uint32_t pages, uint32_t totalMs) {
  if (totalMs == 0) return 0;
  return static_cast<uint32_t>(static_cast<uint64_t>(pages) * 3600000ULL / totalMs);
}

uint32_t avgMsPerPage(uint32_t totalMs, uint32_t pages) {
  if (pages == 0) return 0;
  return totalMs / pages;
}

uint32_t avgMsPerSession(uint32_t totalMs, uint32_t sessions) {
  if (sessions == 0) return 0;
  return totalMs / sessions;
}
```

- [ ] **Step 3: Test** — append to `test/reading_stats/ReadingStatsTest.cpp` (the `using` for the namespace funcs: add `using reading_stats::pagesPerHour; using reading_stats::avgMsPerPage; using reading_stats::avgMsPerSession;` near the other usings):

```cpp
TEST(StatsCompute, PagesPerHour) {
  EXPECT_EQ(pagesPerHour(0, 0), 0u);
  EXPECT_EQ(pagesPerHour(5, 0), 0u);           // no time -> 0
  EXPECT_EQ(pagesPerHour(30, 3600000), 30u);   // 30 pages in 1 h
  EXPECT_EQ(pagesPerHour(29, 858475), 121u);   // 29 * 3600000 / 858475 = 121.6 -> 121
}

TEST(StatsCompute, AveragesGuardZero) {
  EXPECT_EQ(avgMsPerPage(0, 0), 0u);
  EXPECT_EQ(avgMsPerPage(60000, 0), 0u);       // no pages -> 0
  EXPECT_EQ(avgMsPerPage(60000, 4), 15000u);   // 15 s/page
  EXPECT_EQ(avgMsPerSession(0, 0), 0u);
  EXPECT_EQ(avgMsPerSession(60000, 0), 0u);    // no sessions -> 0
  EXPECT_EQ(avgMsPerSession(90000, 3), 30000u);// 30 s/session
}
```

- [ ] **Step 4: Build + run.**
```bash
cd ~/Github/inkpoint
cmake --build build/test --target ReadingStatsTest
./build/test/reading_stats/ReadingStatsTest --gtest_filter='StatsCompute.*'
```
Expected: 2 StatsCompute tests PASS. Then `--gtest_filter='*'` → all pass (95 + 2 = 97), warning-clean. (If a StatsCompute test fails, fix StatsFormat.cpp; do not change tests.)

- [ ] **Step 5: clang-format + commit.**
```bash
clang-format -i --style=file lib/ReadingStats/StatsFormat.h lib/ReadingStats/StatsFormat.cpp test/reading_stats/ReadingStatsTest.cpp
git add lib/ReadingStats/StatsFormat.h lib/ReadingStats/StatsFormat.cpp test/reading_stats/ReadingStatsTest.cpp
git commit -m "Add reading-stats derived-metric helpers"
```

---

## Task 2: i18n strings for the detail page

**Files:** modify `lib/I18n/translations/english.yaml`

- [ ] **Step 1:** After the `STR_READING_STATS_PER_HOUR:` line (added in Plan 2), add:
```yaml
STR_READING_STATS_LABEL_PAGES: "Pages"
STR_READING_STATS_TIME: "Time"
STR_READING_STATS_SESSIONS: "Sessions"
STR_READING_STATS_SPEED: "Speed"
STR_READING_STATS_PER_PAGE: "Per page"
STR_READING_STATS_PER_SESSION: "Per session"
STR_READING_STATS_THIS_BOOK: "This book"
STR_READING_STATS_AVERAGE: "Average"
```
- [ ] **Step 2:** Regenerate + verify:
```bash
cd ~/Github/inkpoint && python3 scripts/gen_i18n.py && grep -c "STR_READING_STATS_LABEL_PAGES\|STR_READING_STATS_THIS_BOOK\|STR_READING_STATS_AVERAGE" lib/I18n/I18nKeys.h
```
Expected: gen runs clean; grep returns 3. (Generated I18n files are build-time/untracked — commit only the yaml unless `git status` shows them tracked+modified.)
- [ ] **Step 3:** Commit:
```bash
git add lib/I18n/translations/english.yaml
git commit -m "Add i18n strings for the reading statistics detail page"
```

---

## Task 3: ReadingStatsDetailActivity (the detail page)

**Files:** create `src/activities/home/ReadingStatsDetailActivity.h`, `src/activities/home/ReadingStatsDetailActivity.cpp`

The detail page is PUSHED from the list with the selected book + the all-books totals (for the average). Back (`finish()`) returns to the list.

- [ ] **Step 1: Header** `src/activities/home/ReadingStatsDetailActivity.h`:

```cpp
#pragma once
#include <ReadingStats.h>

#include "activities/Activity.h"

// Detail page for a single book's reading statistics: full metrics plus a
// comparative bar of this book's speed vs. the all-books average. Read-only;
// Back (finish) returns to the statistics list.
class ReadingStatsDetailActivity final : public Activity {
  reading_stats::BookStats book;
  uint32_t globalPages;
  uint32_t globalMs;

 public:
  ReadingStatsDetailActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, reading_stats::BookStats book,
                             uint32_t globalPages, uint32_t globalMs)
      : Activity("ReadingStatsDetail", renderer, mappedInput),
        book(std::move(book)),
        globalPages(globalPages),
        globalMs(globalMs) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
```

- [ ] **Step 2: Implementation** `src/activities/home/ReadingStatsDetailActivity.cpp`:

```cpp
#include "ReadingStatsDetailActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <StatsFormat.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ReadingStatsDetailActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void ReadingStatsDetailActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void ReadingStatsDetailActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = metrics.contentSidePadding;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 reading_stats::pathToDisplayName(book.bookPath).c_str());

  const uint32_t speed = reading_stats::pagesPerHour(book.pagesRead, book.totalReadingMs);
  const uint32_t avgPage = reading_stats::avgMsPerPage(book.totalReadingMs, book.pagesRead);
  const uint32_t avgSession = reading_stats::avgMsPerSession(book.totalReadingMs, book.sessionCount);
  const uint32_t avgPagesSession = book.sessionCount ? book.pagesRead / book.sessionCount : 0;
  const uint32_t avgSpeed = reading_stats::pagesPerHour(globalPages, globalMs);

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 18;
  const int lineStep = 26;

  auto statLine = [&](const char* label, const std::string& value) {
    const std::string s = std::string(label) + ": " + value;
    renderer.drawText(UI_10_FONT_ID, x, y, s.c_str());
    y += lineStep;
  };

  statLine(tr(STR_READING_STATS_LABEL_PAGES), std::to_string(book.pagesRead));
  statLine(tr(STR_READING_STATS_TIME), reading_stats::formatDurationMs(book.totalReadingMs));
  statLine(tr(STR_READING_STATS_SESSIONS), std::to_string(book.sessionCount));
  statLine(tr(STR_READING_STATS_SPEED), std::to_string(speed) + tr(STR_READING_STATS_PER_HOUR));
  statLine(tr(STR_READING_STATS_PER_PAGE), reading_stats::formatDurationMs(avgPage));
  statLine(tr(STR_READING_STATS_PER_SESSION), reading_stats::formatDurationMs(avgSession) + " - " +
                                                  std::to_string(avgPagesSession) + " " +
                                                  tr(STR_READING_STATS_PAGES));

  // Comparative speed bars: this book vs. all-books average.
  y += 8;
  const uint32_t maxSpeed = std::max({speed, avgSpeed, 1u});
  const int valueColW = 70;
  const int barMaxW = pageWidth - 2 * x - valueColW;
  const int barH = 14;

  auto bar = [&](const char* label, uint32_t value) {
    renderer.drawText(SMALL_FONT_ID, x, y, label);
    y += 18;
    const int w = static_cast<int>(static_cast<int64_t>(barMaxW) * value / maxSpeed);
    renderer.drawRect(x, y, barMaxW, barH);             // outline (full scale)
    renderer.fillRect(x, y, std::max(w, 1), barH);      // filled portion
    const std::string v = std::to_string(value) + tr(STR_READING_STATS_PER_HOUR);
    renderer.drawText(SMALL_FONT_ID, x + barMaxW + 8, y + barH - 2, v.c_str());
    y += barH + 12;
  };

  bar(tr(STR_READING_STATS_THIS_BOOK), speed);
  bar(tr(STR_READING_STATS_AVERAGE), avgSpeed);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
```

> Notes / likely fixes:
> - `STR_BACK`: verify it exists (`grep STR_BACK lib/I18n/I18nKeys.h`). If absent, use the existing "home/back" label string the list screen uses (`STR_HOME`) or add `STR_BACK: "Back"` to english.yaml. Pick whatever the codebase already uses for a back hint.
> - `mapLabels(...)` takes 4 `const char*`; empty `""` args are fine (mirrors the list screen's `""` btn2).
> - `drawRect`/`fillRect` signatures: `fillRect(x,y,w,h,state=true)`, `drawRect(x,y,w,h,state=true)` (confirmed in GfxRenderer.h). `state=true` draws black.
> - If the content overflows a small (X3) screen, it's acceptable for v1 (totals are few lines); the implementer may reduce `lineStep` to fit but should not add scrolling (out of scope).

- [ ] **Step 3: Build.** `cd ~/Github/inkpoint && ~/.pio312/bin/pio run -e default` → `[SUCCESS]`. Fix compile errors (string/`const char*` types in lambdas; missing includes — mirror ReadingStatsActivity.cpp's includes). If `STR_BACK` doesn't exist, resolve per the note. BLOCKED with the exact error if unresolvable.

- [ ] **Step 4: clang-format + commit.**
```bash
clang-format -i --style=file src/activities/home/ReadingStatsDetailActivity.h src/activities/home/ReadingStatsDetailActivity.cpp
git add src/activities/home/ReadingStatsDetailActivity.h src/activities/home/ReadingStatsDetailActivity.cpp
git commit -m "Add reading statistics book detail page"
```

---

## Task 4: Open the detail page from the list

**Files:** modify `src/activities/home/ReadingStatsActivity.h`, `src/activities/home/ReadingStatsActivity.cpp`

- [ ] **Step 1:** In `ReadingStatsActivity.cpp`, add the include with the others:
```cpp
#include "ReadingStatsDetailActivity.h"
```
- [ ] **Step 2:** In `ReadingStatsActivity::loop()`, add a Confirm handler that opens the detail for the selected book. Place it right after the `Back → onGoHome()` block (before the ButtonNavigator handlers):
```cpp
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!books.empty() && selectorIndex < books.size()) {
      startActivityForResult(
          std::make_unique<ReadingStatsDetailActivity>(renderer, mappedInput, books[selectorIndex], totalPages,
                                                        totalMs),
          [](const ActivityResult&) {});
    }
    return;
  }
```
- [ ] **Step 3:** Update the button hints so the middle button shows it opens detail. In `render()`, change the hints line from:
```cpp
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
```
to (use an existing "open/select/view" string — `STR_OPEN` is used by the recent-books list):
```cpp
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
```
(`#include` for `<memory>` may be needed for `std::make_unique` — add it if the build complains; check whether RecentBooksActivity.cpp includes `<memory>` and mirror.)

- [ ] **Step 4: Build.** `~/.pio312/bin/pio run -e default` → `[SUCCESS]`. Confirm `startActivityForResult` and `ActivityResult` are available (Activity base + ActivityResult.h, transitively included). Fix/BLOCK as needed.

- [ ] **Step 5: clang-format + commit.**
```bash
clang-format -i --style=file src/activities/home/ReadingStatsActivity.cpp
git add src/activities/home/ReadingStatsActivity.cpp
git commit -m "Open the book detail page from the statistics list"
```
(`ReadingStatsActivity.h` only changes if a new declaration is needed — it is not, since the open logic lives inline in `loop()`. Do not modify the header.)

---

## Task 5: Pre-PR checks + on-device

- [ ] **Step 1:** `clang-format --dry-run --Werror` on all touched files; `~/.pio312/bin/pio run -t unit-tests` (expect all pass incl. StatsCompute); `~/.pio312/bin/pio run -e default`; `~/.pio312/bin/pio check -e default`. All green.
- [ ] **Step 2 (manual, device):** flash the build, open Home → Reading Statistics → select a book → confirm the detail page shows Pages/Time/Sessions/Speed/Per-page/Per-session and the two comparative speed bars (This book vs Average), and Back returns to the list at the same scroll position.

---

## Self-Review notes
- **Spec coverage:** detail page reachable by selecting a book ✓ (Task 4), full per-book metrics ✓, comparative speed bar ✓ (Task 3), pure metrics host-tested ✓ (Task 1), i18n ✓ (Task 2). Back→list ✓ (finish).
- **Type consistency:** `reading_stats::pagesPerHour/avgMsPerPage/avgMsPerSession` defined in Task 1, tested there, used in Task 3. Detail ctor `(book, globalPages, globalMs)` matches the Task-4 call site. Activity name `"ReadingStatsDetail"` (not in any goHome map — it's a pushed sub-activity, returns via finish()).
- **Out of scope:** time-series/per-session graphs (no stored history, no RTC); XTC/TXT tracking.
