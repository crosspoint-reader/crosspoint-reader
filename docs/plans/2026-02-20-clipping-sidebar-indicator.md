# Clipping Sidebar Indicator Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Show a thin vertical sidebar line on epub pages that are part of a saved clipping, giving visual feedback that the current passage was previously captured.

**Architecture:** At render time, check if the current `(spineIndex, pageIndex)` falls within any saved clipping's range. If so, draw a 3px dark gray vertical line along the left margin before rendering text. Clipping entries are cached in memory on activity enter to avoid SD card reads during render.

**Tech Stack:** C++ / ESP32 / FreeRTOS / GfxRenderer (e-ink)

**Prerequisites:** Rebase `feature/clippings-rework` onto current `origin/master` first (48 new commits). This rebase is a separate prerequisite task — not part of the sidebar feature itself — but is required because master has significantly refactored the Activity base class (render lifecycle, RAII locks, FreeRTOS notifications).

---

## Part 0: Rebase onto upstream master

The upstream `origin/master` has 48 new commits since our branch diverged. Key breaking changes:

1. **`Activity` base class refactor** (`a616f42`): `displayTaskLoop()`, `TaskHandle_t`, `SemaphoreHandle_t`, and `updateRequired` are removed from individual activities. The base class now owns:
   - `renderTaskHandle` + `renderingMutex` (created in `Activity` constructor/`onEnter()`)
   - `renderTaskLoop()` using `ulTaskNotifyTake(pdTRUE, portMAX_DELAY)` instead of polling
   - `render(Activity::RenderLock&&)` virtual method replaces `renderScreen()`
   - `requestUpdate()` replaces `updateRequired = true`

2. **`ActivityWithSubactivity`** now has its own `renderTaskLoop()` that checks `!subActivity` before calling `render()`. It also provides `enterNewActivity()` and `exitActivity()` with built-in locking.

3. **RAII `RenderLock`** (`3d47c08`): All `xSemaphoreTake/Give` pairs replaced with `RenderLock lock(*this)`.

4. **Other changes**: `HalStorage` replaces `SdMan`, `LOG_DBG/LOG_ERR` replaces `Serial.printf`, `I18n` system, `UITheme`, font compression, and more.

## Task 0.1: Rebase and resolve conflicts

**Files affected by conflicts:**
- `src/activities/reader/EpubReaderActivity.h` — our additions (capture state, clipping includes) must merge into master's restructured header
- `src/activities/reader/EpubReaderActivity.cpp` — heavy conflicts: our capture/clipping code layered onto master's refactored render/loop
- `src/activities/reader/EpubReaderMenuActivity.h` — our added menu items (CAPTURE, CLIPPINGS, BOOKMARKS) into master's i18n-ized menu

**New files (no conflict, but must adopt new patterns):**
- `src/activities/reader/EpubReaderClippingsListActivity.h/.cpp`
- `src/activities/reader/ClippingTextViewerActivity.h/.cpp`
- `src/activities/reader/EpubReaderBookmarkListActivity.h/.cpp`
- `src/ClippingStore.h/.cpp`
- `src/BookmarkStore.h/.cpp`

**Step 1: Start rebase**
```bash
git fetch origin master
git rebase origin/master
```

**Step 2: Resolve EpubReaderActivity.h conflicts**

Adopt master's structure. Our additions:
```cpp
// Add to includes (master already has ActivityWithSubactivity)
#include "BookmarkStore.h"
#include "ClippingStore.h"

// Add to private members (after master's existing members)
std::string statusBarOverride;
enum class CaptureState { IDLE, CAPTURING };
CaptureState captureState = CaptureState::IDLE;
std::vector<CapturedPage> captureBuffer;
bool pendingCaptureAfterRender = false;

// Add capture helper declarations
void captureCurrentPage();
void startCapture();
void stopCapture();
void cancelCapture();
void addBookmark();
```

Key difference: Remove our `TaskHandle_t displayTaskHandle`, `SemaphoreHandle_t renderingMutex`, `bool updateRequired` — these now live in the base class.

**Step 3: Resolve EpubReaderActivity.cpp conflicts**

Major adaptations needed:
- `renderScreen()` → `render(Activity::RenderLock&& lock)` override
- `updateRequired = true` → `requestUpdate()`
- `xSemaphoreTake/Give` pairs → `RenderLock lock(*this)`
- `displayTaskLoop()` / `taskTrampoline()` → delete entirely (base class handles this)
- `onEnter()`: call `ActivityWithSubactivity::onEnter()` (which creates render task), remove manual task creation
- `onExit()`: call `ActivityWithSubactivity::onExit()`, remove manual task deletion
- `SdMan` → `Storage` (HalStorage)
- `Serial.printf` → `LOG_DBG` / `LOG_ERR`

Menu opening (capture auto-save) pattern changes from:
```cpp
xSemaphoreTake(renderingMutex, portMAX_DELAY);
if (captureState == CaptureState::CAPTURING) { stopCapture(); ... }
enterNewActivity(new EpubReaderMenuActivity(...));
xSemaphoreGive(renderingMutex);
```
to:
```cpp
if (captureState == CaptureState::CAPTURING) { stopCapture(); requestUpdate(); vTaskDelay(400 / portTICK_PERIOD_MS); }
// enterNewActivity already acquires RenderLock internally
enterNewActivity(new EpubReaderMenuActivity(...));
```

**Step 4: Resolve EpubReaderMenuActivity.h conflicts**

Master uses `StrId` enum for i18n labels. Our added menu items need `StrId` entries. If `StrId` entries for CAPTURE/CLIPPINGS/BOOKMARKS don't exist, use raw string labels temporarily and add i18n in a follow-up.

**Step 5: Adapt new activity files to master patterns**

For each of `EpubReaderClippingsListActivity`, `ClippingTextViewerActivity`, `EpubReaderBookmarkListActivity`:

Headers — remove:
```cpp
TaskHandle_t displayTaskHandle = nullptr;
SemaphoreHandle_t renderingMutex = nullptr;
bool updateRequired = false;
static void taskTrampoline(void* param);
[[noreturn]] void displayTaskLoop();
void renderScreen();
```
Add:
```cpp
void render(Activity::RenderLock&&) override;
```

Implementation — remove:
- `taskTrampoline()` function
- `displayTaskLoop()` function
- Manual mutex creation/deletion in `onEnter()`/`onExit()`
- Manual task creation/deletion in `onEnter()`/`onExit()`

Replace:
- `onEnter()`: call `ActivityWithSubactivity::onEnter()` (or `Activity::onEnter()` for non-subactivity), then do activity-specific init, then `requestUpdate()`
- `onExit()`: call `ActivityWithSubactivity::onExit()` (or `Activity::onExit()`)
- `renderScreen()` → `render(Activity::RenderLock&& lock)` with same body
- `updateRequired = true` → `requestUpdate()`
- All `xSemaphoreTake/Give` → `RenderLock lock(*this)` (or remove if base handles it)
- `SdMan` → `Storage`

**Step 6: Adapt ClippingStore/BookmarkStore to master APIs**

Replace:
- `SdMan.openFileForRead(TAG, path, file)` → `Storage.openFileForRead(TAG, path, file)`
- `SdMan.openFileForWrite(TAG, path, file)` → `Storage.openFileForWrite(TAG, path, file)`
- `Serial.printf(...)` → `LOG_DBG(TAG, ...)` / `LOG_ERR(TAG, ...)`

**Step 7: Build and verify**
```bash
pio run -e default
```
Expected: Clean build with no errors.

**Step 8: Flash and smoke test**
```bash
pio run -e default --upload-port /dev/cu.usbmodem101 -t upload
```
Verify: Open a book, capture a clipping, view clippings list, open clipping detail, delete a clipping, add a bookmark, view bookmarks.

**Step 9: Fix remaining cppcheck issue**

The `useStlAlgorithm` suppression at `ClippingStore.cpp` that inline `cppcheck-suppress` failed to fix. Replace the raw loop with `std::min_element`:
```cpp
#include <algorithm>
// ...
auto it = std::min_element(entries.begin(), entries.end(),
    [](const ClippingEntry& a, const ClippingEntry& b) { return a.textOffset < b.textOffset; });
uint32_t minOffset = (it != entries.end()) ? it->textOffset : origFile.size();
```

**Step 10: Commit rebase result**
```bash
git add -A
git rebase --continue
# Force push since rebase rewrites history
git push --force-with-lease
```

---

## Part 1: Clipping sidebar indicator

## Task 1.1: Add `hasClippingAtPage` query to ClippingStore

**Files:**
- Modify: `src/ClippingStore.h`
- Modify: `src/ClippingStore.cpp`

**Step 1: Add method declaration to header**

In `ClippingStore.h`, add to public section:
```cpp
// Check if any clipping covers the given page.
static bool hasClippingAtPage(const std::vector<ClippingEntry>& entries,
                              uint16_t spineIndex, uint16_t pageIndex);
```

**Step 2: Implement the method**

In `ClippingStore.cpp`:
```cpp
bool ClippingStore::hasClippingAtPage(const std::vector<ClippingEntry>& entries,
                                      uint16_t spineIndex, uint16_t pageIndex) {
  for (const auto& e : entries) {
    if (e.spineIndex == spineIndex && pageIndex >= e.startPage && pageIndex <= e.endPage) {
      return true;
    }
  }
  return false;
}
```

Note: Takes pre-loaded entries vector rather than reading from SD, since `EpubReaderActivity` will cache the index.

**Step 3: Build**
```bash
pio run -e default
```

**Step 4: Commit**
```bash
git add src/ClippingStore.h src/ClippingStore.cpp
git commit -m "feat: add hasClippingAtPage query to ClippingStore"
```

## Task 1.2: Cache clipping index in EpubReaderActivity

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.h`
- Modify: `src/activities/reader/EpubReaderActivity.cpp`

**Step 1: Add cached entries member**

In `EpubReaderActivity.h`, add to private members:
```cpp
std::vector<ClippingEntry> cachedClippings;  // Cached clipping index for sidebar indicator
```

**Step 2: Load clipping index on enter**

In `EpubReaderActivity::onEnter()`, after the epub path is available:
```cpp
cachedClippings = ClippingStore::loadIndex(epub->getPath());
```

**Step 3: Refresh cache after capture saves**

In `stopCapture()`, after `ClippingStore::saveClipping(...)` succeeds:
```cpp
cachedClippings = ClippingStore::loadIndex(epub->getPath());
```

**Step 4: Refresh cache when returning from clippings list (in case user deleted)**

In the clippings list onGoBack callback:
```cpp
cachedClippings = ClippingStore::loadIndex(epub->getPath());
```

**Step 5: Build**
```bash
pio run -e default
```

**Step 6: Commit**
```bash
git add src/activities/reader/EpubReaderActivity.h src/activities/reader/EpubReaderActivity.cpp
git commit -m "feat: cache clipping index in reader for sidebar indicator"
```

## Task 1.3: Draw sidebar line in render()

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.cpp`

**Step 1: Add sidebar drawing logic**

In `EpubReaderActivity::render()`, after margin calculation and before `renderContents()`, add:
```cpp
// Draw clipping sidebar indicator
if (!cachedClippings.empty() && section) {
  if (ClippingStore::hasClippingAtPage(cachedClippings,
      static_cast<uint16_t>(currentSpineIndex),
      static_cast<uint16_t>(section->currentPage))) {
    const int sidebarX = orientedMarginLeft - 6;
    const int sidebarY = orientedMarginTop;
    const int sidebarHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
    renderer.fillRect(sidebarX, sidebarY, 3, sidebarHeight, true);
  }
}
```

Note: `fillRect(..., true)` draws black pixels. On e-ink this gives a clear, crisp sidebar line. If it's too bold, can switch to `fillRectDither(..., GfxRenderer::Color::DarkGray)` for a softer look.

**Step 2: Build and flash**
```bash
pio run -e default --upload-port /dev/cu.usbmodem101 -t upload
```

**Step 3: Test on device**
- Open a book
- Capture a multi-page clipping (pages 3-5)
- Navigate back through pages 3, 4, 5 — sidebar line should appear on each
- Navigate to pages before/after the clipping — no sidebar
- Capture another clipping on an overlapping page — still single sidebar line
- Delete a clipping from the list, return to reader — sidebar should be gone

**Step 4: Commit**
```bash
git add src/activities/reader/EpubReaderActivity.cpp
git commit -m "feat: draw sidebar line on pages with saved clippings"
```

## Task 1.4: Handle landscape orientation

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.cpp`

**Step 1: Adjust sidebar position for landscape**

The sidebar should always be on the "left" edge relative to reading direction. In landscape modes the margins rotate. The `orientedMarginLeft` already accounts for this, so the same `orientedMarginLeft - 6` should work. However, verify on-device that the sidebar is visible and not clipped by the display edge in both landscape orientations.

If the hint gutter is active in landscape, offset the sidebar to avoid overlap:
```cpp
const auto orientation = renderer.getOrientation();
const bool isLandscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                         orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
const int gutterOffset = isLandscape ? 30 : 0;
const int sidebarX = orientedMarginLeft + gutterOffset - 6;
```

**Step 2: Flash and test in both orientations**

**Step 3: Commit if changes were needed**
```bash
git add src/activities/reader/EpubReaderActivity.cpp
git commit -m "fix: adjust sidebar position for landscape orientations"
```

## Task 1.5: Push and verify CI

**Step 1: Push**
```bash
git push --force-with-lease
```

**Step 2: Verify CI passes**
- build: should pass
- clang-format: run locally first: `/opt/homebrew/opt/llvm/bin/clang-format --dry-run -Werror <changed-files>`
- cppcheck: ensure no new warnings from our changes
