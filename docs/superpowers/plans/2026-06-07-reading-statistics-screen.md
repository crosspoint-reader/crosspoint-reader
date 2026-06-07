# Reading Statistics Screen Implementation Plan (Plan 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add an on-device **Reading Statistics** screen, reachable from a new top-level **Home menu** entry, showing all-time totals plus a scrollable per-book list (pages, reading time, speed).

**Architecture:** Builds on Plan 1 (the `ReadingStatsStore` / `READING_STATS` singleton already tracks and persists stats). Adds: (1) pure host-tested formatting helpers in `lib/ReadingStats/StatsFormat.{h,cpp}`; (2) a new `ReadingStatsActivity` list screen mirroring `RecentBooksActivity`; (3) i18n strings; (4) Home-menu + `ActivityManager` wiring. Pure helpers are unit-tested; the screen/menu are verified by firmware build + on-device.

**Tech Stack:** C++20, PlatformIO (ESP32-C3), GoogleTest (host), e-ink GUI via `GUI.drawList`/`drawHeader`/`drawButtonHints`, i18n via `tr(STR_*)` generated from `lib/I18n/translations/english.yaml`.

**Build/test commands (this machine):**
- Firmware: `~/.pio312/bin/pio run -e default` (the Homebrew `pio` is broken — Python 3.14; use the venv one).
- Host tests: `cmake --build build/test --target ReadingStatsTest && ./build/test/reading_stats/ReadingStatsTest --gtest_filter='*'`
- clang-format: `clang-format -i --style=file <files>` (v22; repo accepts ≥21).

**Commit convention:** plain messages, **no `Co-Authored-By` trailer**, no AI attribution.

---

## File Structure

| File | Responsibility | Tested |
|------|----------------|--------|
| `lib/ReadingStats/StatsFormat.h` / `.cpp` | pure `formatDurationMs` + `pathToDisplayName` | host gtest |
| `test/reading_stats/ReadingStatsTest.cpp` | add StatsFormat cases (modify) | — |
| `test/reading_stats/CMakeLists.txt` | add StatsFormat.cpp to the test exe (modify) | — |
| `lib/I18n/translations/english.yaml` | new UI strings (modify) | — |
| `src/activities/home/ReadingStatsActivity.h` / `.cpp` | the new screen | manual |
| `src/activities/ActivityManager.h` / `.cpp` | `HomeMenuItem::READING_STATS`, `goToReadingStats()`, name map (modify) | manual |
| `src/activities/home/HomeActivity.h` / `.cpp` | menu entry wiring (modify) | manual |

---

## Task 1: Pure formatting helpers (TDD, host)

**Files:**
- Create: `lib/ReadingStats/StatsFormat.h`, `lib/ReadingStats/StatsFormat.cpp`
- Modify: `test/reading_stats/CMakeLists.txt`, `test/reading_stats/ReadingStatsTest.cpp`

- [ ] **Step 1: Create the header** `lib/ReadingStats/StatsFormat.h`:

```cpp
#pragma once
#include <cstdint>
#include <string>

namespace reading_stats {

// Format a millisecond duration for display. >= 1 hour -> "Xh Ym"; otherwise "Ym Zs".
std::string formatDurationMs(uint32_t ms);

// Derive a display name from a book file path: basename without the extension.
// "/books/great-gatsby.epub" -> "great-gatsby". A path with no '/' or no '.' is
// handled gracefully (returns the filename, or the whole string).
std::string pathToDisplayName(const std::string& path);

}  // namespace reading_stats
```

- [ ] **Step 2: Create** `lib/ReadingStats/StatsFormat.cpp`:

```cpp
#include "StatsFormat.h"

#include <cstdio>

namespace reading_stats {

std::string formatDurationMs(uint32_t ms) {
  const uint32_t totalSec = ms / 1000;
  const uint32_t hours = totalSec / 3600;
  const uint32_t mins = (totalSec % 3600) / 60;
  char buf[32];
  if (hours > 0) {
    std::snprintf(buf, sizeof(buf), "%uh %um", static_cast<unsigned>(hours), static_cast<unsigned>(mins));
  } else {
    const uint32_t secs = totalSec % 60;
    std::snprintf(buf, sizeof(buf), "%um %us", static_cast<unsigned>(mins), static_cast<unsigned>(secs));
  }
  return buf;
}

std::string pathToDisplayName(const std::string& path) {
  const auto slashPos = path.rfind('/');
  std::string filename = (slashPos != std::string::npos) ? path.substr(slashPos + 1) : path;
  const auto dotPos = filename.rfind('.');
  // Keep the whole name if there's no extension, or if the only dot is a leading
  // one (a dotfile like ".hidden" should not collapse to an empty string).
  if (dotPos == std::string::npos || dotPos == 0) {
    return filename;
  }
  return filename.substr(0, dotPos);
}

}  // namespace reading_stats
```

- [ ] **Step 3: Wire the new source into the test executable.** In `test/reading_stats/CMakeLists.txt`, add `StatsFormat.cpp` to the `add_executable` sources:

```cmake
add_executable(ReadingStatsTest
  ReadingStatsTest.cpp
  ${REPO_ROOT}/lib/ReadingStats/ReadingStats.cpp
  ${REPO_ROOT}/lib/ReadingStats/StatsFormat.cpp
)
```

- [ ] **Step 4: Write the failing tests.** Append to `test/reading_stats/ReadingStatsTest.cpp`:

```cpp
#include "StatsFormat.h"

using reading_stats::formatDurationMs;
using reading_stats::pathToDisplayName;

TEST(StatsFormat, FormatsDurationUnderOneHourAsMinutesSeconds) {
  EXPECT_EQ(formatDurationMs(0), "0m 0s");
  EXPECT_EQ(formatDurationMs(37606), "0m 37s");     // ~37.6 s
  EXPECT_EQ(formatDurationMs(668754), "11m 8s");    // 11 min 8.75 s
  EXPECT_EQ(formatDurationMs(59999), "0m 59s");     // just under a minute
}

TEST(StatsFormat, FormatsDurationOverOneHourAsHoursMinutes) {
  EXPECT_EQ(formatDurationMs(3600000), "1h 0m");          // exactly 1 h
  EXPECT_EQ(formatDurationMs(3600000 + 125000), "1h 2m"); // 1 h 2 min 5 s
  EXPECT_EQ(formatDurationMs(36000000), "10h 0m");        // 10 h
}

TEST(StatsFormat, DerivesDisplayNameFromPath) {
  EXPECT_EQ(pathToDisplayName("/books/great-gatsby.epub"), "great-gatsby");
  EXPECT_EQ(pathToDisplayName("/a/b/c.txt"), "c");
  EXPECT_EQ(pathToDisplayName("plath-bell-jar.epub"), "plath-bell-jar");
  EXPECT_EQ(pathToDisplayName("noextension"), "noextension");
  EXPECT_EQ(pathToDisplayName("/dir/file.name.epub"), "file.name");  // last dot only
}
```

- [ ] **Step 5: Configure (picks up the new source) and run — expect PASS.**

Run:
```bash
cd ~/Github/inkpoint
cmake -S test -B build/test -DCMAKE_BUILD_TYPE=Release
cmake --build build/test --target ReadingStatsTest
./build/test/reading_stats/ReadingStatsTest --gtest_filter='StatsFormat.*'
```
Expected: 3 StatsFormat tests PASS. (The implementation is written, so they pass on first run; if any fail, fix `StatsFormat.cpp` — do not change the tests.) Then run the full suite `--gtest_filter='*'` and confirm all pass (12 ReadingStats + 3 StatsFormat = 15), warning-clean.

- [ ] **Step 6: clang-format + commit.**
```bash
clang-format -i --style=file lib/ReadingStats/StatsFormat.h lib/ReadingStats/StatsFormat.cpp test/reading_stats/ReadingStatsTest.cpp
git add lib/ReadingStats/StatsFormat.h lib/ReadingStats/StatsFormat.cpp test/reading_stats/CMakeLists.txt test/reading_stats/ReadingStatsTest.cpp
git commit -m "Add reading-stats display formatting helpers"
```

---

## Task 2: i18n strings

**Files:**
- Modify: `lib/I18n/translations/english.yaml`

Only English is required; other languages fall back to English automatically (`gen_i18n.py`). The build regenerates `lib/I18n/I18nKeys.h` / `I18nStrings.*` — do NOT edit those generated files.

- [ ] **Step 1: Add the strings.** In `lib/I18n/translations/english.yaml`, find the line `STR_MENU_RECENT_BOOKS: "Recent Books"` and add the following block immediately after it (keep the file's 2-space style and quoting):

```yaml
STR_READING_STATS_TITLE: "Reading Statistics"
STR_NO_READING_STATS: "No reading statistics yet"
STR_READING_STATS_TOTAL: "Total"
STR_READING_STATS_PAGES: "pages"
STR_READING_STATS_BOOKS: "books"
STR_READING_STATS_PER_HOUR: "/h"
```

- [ ] **Step 2: Regenerate + build to confirm the keys exist.**

Run:
```bash
cd ~/Github/inkpoint
python3 scripts/gen_i18n.py
grep -n "STR_READING_STATS_TITLE\|STR_NO_READING_STATS\|STR_READING_STATS_PER_HOUR" lib/I18n/I18nKeys.h
```
Expected: `gen_i18n.py` runs without error and the grep shows the new `StrId` entries in the generated `I18nKeys.h`. (If `gen_i18n.py` needs args, inspect its `--help`; the default reads `lib/I18n/translations` and writes `lib/I18n/`.)

- [ ] **Step 3: Commit.**

The generated `I18nKeys.h` / `I18nStrings.*` are tracked in the repo, so commit them alongside the yaml.
```bash
git add lib/I18n/translations/english.yaml lib/I18n/I18nKeys.h lib/I18n/I18nStrings.h lib/I18n/I18nStrings.cpp
git commit -m "Add i18n strings for the reading statistics screen"
```
(If `git status` shows other generated I18n files changed by the regeneration, add those too; if NONE of the generated files are tracked/changed — i.e. they're produced only at build time — then commit just the yaml. Check `git status` and adapt.)

---

## Task 3: ReadingStatsActivity (the screen)

**Files:**
- Create: `src/activities/home/ReadingStatsActivity.h`, `src/activities/home/ReadingStatsActivity.cpp`

This mirrors `src/activities/home/RecentBooksActivity.{h,cpp}` (read it first), minus the long-press/remove and book-open behaviour. It shows a totals block then a scrollable per-book list. Verified by firmware build (not host-tested).

- [ ] **Step 1: Create the header** `src/activities/home/ReadingStatsActivity.h`:

```cpp
#pragma once
#include <ReadingStats.h>

#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Top-level screen showing all-time reading totals and a scrollable per-book list
// (pages, time, reading speed). Read-only; Back returns Home.
class ReadingStatsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  size_t selectorIndex = 0;
  std::vector<reading_stats::BookStats> books;  // snapshot, sorted by time desc
  uint32_t totalPages = 0;
  uint32_t totalMs = 0;

  void loadStats();

 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
```

- [ ] **Step 2: Create** `src/activities/home/ReadingStatsActivity.cpp`:

```cpp
#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <StatsFormat.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Vertical space reserved above the list for the totals block (two text rows).
constexpr int TOTALS_BLOCK_HEIGHT = 52;
}  // namespace

void ReadingStatsActivity::loadStats() {
  books = READING_STATS.books();  // copy snapshot
  // Most-read first.
  std::sort(books.begin(), books.end(),
            [](const reading_stats::BookStats& a, const reading_stats::BookStats& b) {
              return a.totalReadingMs > b.totalReadingMs;
            });
  totalPages = READING_STATS.totalPagesRead();
  totalMs = READING_STATS.totalReadingMs();
}

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  loadStats();
  selectorIndex = 0;
  requestUpdate();
}

void ReadingStatsActivity::onExit() {
  Activity::onExit();
  books.clear();
}

void ReadingStatsActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true,
                                                                       TOTALS_BLOCK_HEIGHT);

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  const int listSize = static_cast<int>(books.size());

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_STATS_TITLE));

  const int headerBottom = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // Totals block (two lines).
  const std::string line1 = std::string(tr(STR_READING_STATS_TOTAL)) + ": " + std::to_string(totalPages) + " " +
                            tr(STR_READING_STATS_PAGES);
  const std::string line2 =
      reading_stats::formatDurationMs(totalMs) + "  -  " + std::to_string(books.size()) + " " +
      tr(STR_READING_STATS_BOOKS);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, headerBottom + 14, line1.c_str());
  renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, headerBottom + 36, line2.c_str());

  const int contentTop = headerBottom + TOTALS_BLOCK_HEIGHT;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (books.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_READING_STATS));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(books.size()),
        static_cast<int>(selectorIndex),
        // title: book display name
        [this](int index) { return reading_stats::pathToDisplayName(books[index].bookPath); },
        // subtitle: "<pages> pages - <duration>"
        [this](int index) {
          return std::to_string(books[index].pagesRead) + " " + tr(STR_READING_STATS_PAGES) + " - " +
                 reading_stats::formatDurationMs(books[index].totalReadingMs);
        },
        // icon
        [](int) { return Book; },
        // value (right-aligned): "<speed>/h"
        [this](int index) {
          const auto& b = books[index];
          const uint32_t pph =
              b.totalReadingMs ? static_cast<uint32_t>(static_cast<uint64_t>(b.pagesRead) * 3600000ULL /
                                                       b.totalReadingMs)
                               : 0u;
          return std::to_string(pph) + tr(STR_READING_STATS_PER_HOUR);
        });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
```

> Notes:
> - `getNumberOfItemsPerPage(renderer, hasHeader=true, hasTabBar=false, hasButtonHints=true, hasSubtitle=true, extraReservedHeight=TOTALS_BLOCK_HEIGHT)` reserves the totals block so the page-size math matches the reduced list rect.
> - `drawList` auto-paginates (it derives the page window from `selectedIndex` and the rect height) and draws up/down indicators when there's more than one page — no manual pagination needed.
> - `Book` is a `UIIcon` enum value (from `components/themes/BaseTheme.h`, transitively included via `UITheme.h`).
> - `tr(...)` returns `const char*`; `std::string(tr(...))` / `+ tr(...)` concatenations are fine.
> - ASCII `-` separators are used (not a unicode middle dot) to avoid any font-glyph gaps.

- [ ] **Step 3: Build to verify.** (The menu wiring is Task 4; this task just needs the activity to compile. It will not yet be reachable.)
```bash
cd ~/Github/inkpoint && ~/.pio312/bin/pio run -e default
```
Expected: `[SUCCESS]`. Fix compile errors (check the `mapLabels` empty-string arg compiles — it takes `const char*`; pass `""`). If `drawList`'s lambda return types mismatch (it expects `std::string` for title/subtitle/value and `UIIcon` for icon), adjust the lambdas to return those exact types. If unresolvable, report BLOCKED with the error.

- [ ] **Step 4: clang-format + commit.**
```bash
clang-format -i --style=file src/activities/home/ReadingStatsActivity.h src/activities/home/ReadingStatsActivity.cpp
git add src/activities/home/ReadingStatsActivity.h src/activities/home/ReadingStatsActivity.cpp
git commit -m "Add reading statistics screen activity"
```

---

## Task 4: Home menu + ActivityManager wiring

**Files:**
- Modify: `src/activities/ActivityManager.h`, `src/activities/ActivityManager.cpp`
- Modify: `src/activities/home/HomeActivity.h`, `src/activities/home/HomeActivity.cpp`

Adds `HomeMenuItem::READING_STATS`, the `goToReadingStats()` navigation, the name→enum mapping (so Back returns the cursor to this entry), and the Home-menu entry itself (label + icon + index math + dispatch). Read the current code first to anchor each edit.

- [ ] **Step 1: ActivityManager.h — enum + declaration + include.**

Add `READING_STATS` to the `HomeMenuItem` enum (it must be the LAST value, matching the menu-index math which appends it after Settings):
```cpp
enum class HomeMenuItem { NONE, FILE_BROWSER, RECENTS, OPDS_BROWSER, FILE_TRANSFER, SETTINGS_MENU, READING_STATS };
```
Add the navigation declaration next to `goToRecentBooks();`:
```cpp
void goToReadingStats();
```

- [ ] **Step 2: ActivityManager.cpp — include, navigation impl, name map.**

Add the include with the other activity includes:
```cpp
#include "home/ReadingStatsActivity.h"
```
Add the implementation after `goToRecentBooks()`:
```cpp
void ActivityManager::goToReadingStats() {
  replaceActivity(std::make_unique<ReadingStatsActivity>(renderer, mappedInput));
}
```
In `goHome(...)`, find the block that maps an activity name string to a `HomeMenuItem` (the chain of `if (activityName == "...")`). Add a branch so returning from the stats screen restores the cursor onto its entry:
```cpp
  } else if (activityName == "ReadingStats") {
    initialMenuItem = HomeMenuItem::READING_STATS;
```
(Match the exact `if/else if` shape already present; the activity's name is `"ReadingStats"` from its constructor.)

- [ ] **Step 3: HomeActivity.h — index maps + handler declaration.**

In `menuItemToIndex`, append before the final `return 0;`:
```cpp
  ++i;
  if (item == HomeMenuItem::READING_STATS) return i;
  return 0;
```
In `indexToMenuItem`, change the Settings line to post-increment and add the stats line:
```cpp
  if (idx == i++) return HomeMenuItem::SETTINGS_MENU;
  if (idx == i) return HomeMenuItem::READING_STATS;
  return HomeMenuItem::NONE;
```
Add the private handler declaration alongside the other `onXxxOpen()` declarations:
```cpp
  void onReadingStatsOpen();
```

- [ ] **Step 4: HomeActivity.cpp — count, menu vectors, dispatch, handler.**

In `getMenuItemCount()`, bump the base count and update the comment:
```cpp
  int count = 5;  // File Browser, Recents, File transfer, Settings, Reading Stats
```
In `render()`, append the new label + icon to the initial vectors (the OPDS `begin()+2` and Continue-Reading `begin()` inserts still target fixed front positions, so appending to the tail is safe):
```cpp
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE), tr(STR_READING_STATS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings, Book};
```
In the `loop()` dispatch switch, add a case before `default:`:
```cpp
        case HomeMenuItem::READING_STATS:
          onReadingStatsOpen();
          break;
```
Add the handler next to the other `onXxxOpen()` definitions:
```cpp
void HomeActivity::onReadingStatsOpen() { activityManager.goToReadingStats(); }
```

- [ ] **Step 5: Build to verify.**
```bash
cd ~/Github/inkpoint && ~/.pio312/bin/pio run -e default
```
Expected: `[SUCCESS]`. The compiler will flag the `switch` if the new enum case is missing anywhere a `HomeMenuItem` switch is exhaustively handled — address any `-Wswitch` warning by handling `READING_STATS`. Fix compile errors; if unresolvable, report BLOCKED with the error.

- [ ] **Step 6: clang-format + commit.**
```bash
clang-format -i --style=file src/activities/ActivityManager.h src/activities/ActivityManager.cpp src/activities/home/HomeActivity.h src/activities/home/HomeActivity.cpp
git add src/activities/ActivityManager.h src/activities/ActivityManager.cpp src/activities/home/HomeActivity.h src/activities/home/HomeActivity.cpp
git commit -m "Add Reading Statistics entry to the home menu"
```

---

## Task 5: Pre-PR checks + on-device verification

**Files:** none (verification only)

- [ ] **Step 1: Full local checks.**
```bash
cd ~/Github/inkpoint
clang-format --dry-run --Werror --style=file lib/ReadingStats/StatsFormat.h lib/ReadingStats/StatsFormat.cpp src/activities/home/ReadingStatsActivity.h src/activities/home/ReadingStatsActivity.cpp src/activities/ActivityManager.h src/activities/ActivityManager.cpp src/activities/home/HomeActivity.h src/activities/home/HomeActivity.cpp
~/.pio312/bin/pio run -t unit-tests
~/.pio312/bin/pio run -e default
~/.pio312/bin/pio check -e default
```
Expected: clang-format clean, all host suites pass (now includes the 3 StatsFormat cases), firmware builds, cppcheck no defects.

- [ ] **Step 2: Flash + on-device smoke test (manual).**

Build the `.bin` (already produced at `.pio/build/default/firmware.bin`), copy it to the SD card root, insert into the device, boot holding **UP + power** → SD Card Firmware Update → pick the `.bin` → confirm. Then:
1. From **Home**, scroll to **Reading Statistics** → open it.
2. Confirm the **totals** line (total pages + total time + book count) and a **per-book list** with each book's name, "<pages> pages - <time>", and "<speed>/h" on the right.
3. Scroll the list (up/down; page jump on hold). Confirm pagination arrows appear if the list exceeds one screen.
4. **Back** returns to Home with the cursor on the Reading Statistics entry.
5. Cross-check the numbers against `/.crosspoint/reading_stats.json`.

---

## Self-Review notes

- **Spec coverage:** Home-menu entry ✓ (Task 4), totals + per-book list ✓ (Task 3), reading speed per book ✓, scroll/pagination ✓ (drawList), Back→Home ✓, i18n ✓ (Task 2), pure formatting host-tested ✓ (Task 1).
- **Type consistency:** `reading_stats::formatDurationMs` / `pathToDisplayName` used identically in Task 1 (def/test) and Task 3 (screen). `HomeMenuItem::READING_STATS` is the last enum value, consistent across the index math in `menuItemToIndex`/`indexToMenuItem`/`getMenuItemCount`/render vectors/loop switch. Activity name string `"ReadingStats"` matches between the constructor and the `goHome` name map.
- **No placeholders:** every step has full code or exact commands.
- **Out of scope (unchanged from Plan 1):** XTC/TXT reader tracking; daily streaks. A dedicated stats icon (currently reusing `Book`) could be a later polish.
