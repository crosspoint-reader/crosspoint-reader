# Clippings Rework Implementation Plan

## Overview

Rework the clippings feature to use hidden storage, a dedicated clippings viewer UI, and a simplified capture flow where opening the menu auto-saves. This addresses feedback that the original implementation felt "half-baked" — now clippings get a proper custom UI with text previews, hidden storage (no recents pollution), and a cleaner capture UX.

## Current State

On the `feature/bookmarks-and-clippings-wip` branch we already have:
- `PageExporter` writing markdown to `/clippings/<book>.md` (visible, pollutes recents)
- Capture state machine in `EpubReaderActivity` (start/stop/cancel, forward page appends, backward page stops)
- `CapturedPage` struct with pageText, chapterTitle, bookPercent, chapterPercent
- `VIEW_CLIPPINGS` menu action that opens the `.md` file via `onOpenFile` callback through `ReaderActivity`
- Menu shows "Capture Clipping" / "Stop Capture" toggle

## Desired End State

- **Storage**: `/.crosspoint/clippings/<book-filename>.idx` (binary index) + `/.crosspoint/clippings/<book-filename>.md` (formatted markdown). Both hidden from device file browser.
- **Capture flow**: Menu → "Capture" → status bar shows `"Capturing..."` → forward page turns append text → opening menu auto-saves capture → "Clipping saved" flash → normal menu appears. Backward turn also saves. Back button cancels.
- **Menu order**: Go to Chapter | Bookmarks | Clippings | Capture | ...
- **Clippings viewer**: Dedicated `EpubReaderClippingsListActivity` showing text preview per clipping, with deletion hint. Selecting a clipping opens a scrollable text viewer as a subactivity. Reading position is fully preserved (subactivity stack).
- **No `onOpenFile` callback needed** — clippings viewer is a subactivity, not a separate reader.

### Verification

1. Build passes: `pio run`
2. Flash to device: `pio run --target upload --upload-port /dev/cu.usbmodem2101`
3. Capture a single page (start capture, immediately open menu) → "Clipping saved" → clipping appears in list
4. Capture multiple pages (start capture, turn forward 3 pages, open menu) → saves all pages
5. Backward turn during capture → saves captured pages
6. Clippings list shows text preview truncated with `...`
7. Selecting clipping opens scrollable text viewer
8. Long-press Confirm on clipping shows delete confirmation with hint
9. Exiting clippings viewer returns to exact reading position
10. `.md` file at `/.crosspoint/clippings/` is well-formatted for export
11. No clippings files appear in recents or file browser

## What We're NOT Doing

- Sentence-level selection / highlighting (future work per @kurtisgrant's suggestion)
- "Jump to clipping location in book" action (reading position concern makes this complex)
- Clippings across books (each book has its own clippings)
- Sharing clippings between devices

## Implementation Approach

Reuse the existing capture state machine (it's solid), but:
1. Replace `PageExporter` with `ClippingStore` that writes both `.idx` and `.md`
2. Auto-save on menu open (simplify flow, remove "Stop Capture" menu item)
3. Replace `VIEW_CLIPPINGS` → `onOpenFile` pathway with a proper `EpubReaderClippingsListActivity` subactivity
4. Add `ClippingTextViewerActivity` for viewing full clipping text
5. Remove `onOpenFile` callback from `EpubReaderActivity` and `ReaderActivity`

---

## Phase 1: ClippingStore — Storage Layer

### Overview
Replace `PageExporter` with `ClippingStore` that manages two files per book: a binary `.idx` for fast list loading and a `.md` for human-readable export.

### Changes Required:

#### 1. New file: `src/ClippingStore.h`

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Metadata for a single clipping (stored in .idx file).
struct ClippingEntry {
  uint32_t textOffset;   // byte offset into .md where this clipping's text starts
  uint32_t textLength;   // byte length of the clipping text in .md
  uint8_t bookPercent;   // 0-100 overall book progress at capture start
  uint8_t chapterPercent;// 0-100 chapter progress at capture start
  uint16_t spineIndex;   // spine item where capture started
  uint16_t startPage;    // first page captured (within spine item)
  uint16_t endPage;      // last page captured (within spine item)
};

// A single captured page with its metadata (used during capture, before saving).
struct CapturedPage {
  std::string pageText;
  std::string chapterTitle;
  int bookPercent;
  int chapterPercent;
  uint16_t spineIndex;
  uint16_t pageIndex;
};

// Stores clippings as two files per book:
//   /.crosspoint/clippings/<book-filename>.idx  (binary index)
//   /.crosspoint/clippings/<book-filename>.md   (formatted markdown for export)
//
// Index format: [magic:4 "CIDX"][version:1][count:2 LE][entries: count * 16 bytes]
// Each entry: [textOffset:4 LE][textLength:4 LE][bookPercent:1][chapterPercent:1]
//             [spineIndex:2 LE][startPage:2 LE][endPage:2 LE]
class ClippingStore {
 public:
  // Save a new clipping (appends to both .idx and .md). Returns true on success.
  static bool saveClipping(const std::string& bookPath, const std::string& bookTitle,
                           const std::string& bookAuthor, const std::vector<CapturedPage>& pages);

  // Load clipping index entries for a book.
  static std::vector<ClippingEntry> loadIndex(const std::string& bookPath);

  // Load the full text of a specific clipping from the .md file.
  static std::string loadClippingText(const std::string& bookPath, const ClippingEntry& entry);

  // Load a short preview of a clipping (first N characters).
  static std::string loadClippingPreview(const std::string& bookPath, const ClippingEntry& entry, int maxChars = 60);

  // Delete a clipping at the given index. Rewrites both files.
  static bool deleteClipping(const std::string& bookPath, int index);

  // Get the index file path for a book.
  static std::string getIndexPath(const std::string& bookPath);

  // Get the markdown file path for a book.
  static std::string getMdPath(const std::string& bookPath);

 private:
  static std::string getBasePath(const std::string& bookPath);
  static bool writeIndex(const std::string& path, const std::vector<ClippingEntry>& entries);
  static constexpr const char* CLIPPINGS_DIR = "/.crosspoint/clippings";
  static constexpr uint8_t FORMAT_VERSION = 1;
  static constexpr const char* INDEX_MAGIC = "CIDX";
  static constexpr const char* TAG = "CLP";
};
```

#### 2. New file: `src/ClippingStore.cpp`

Key implementation details:
- `getBasePath()`: FNV-1a hash of full bookPath (same pattern as `BookmarkStore::getBookmarkPath`), return `CLIPPINGS_DIR + "/" + hexHash`
- `getIndexPath()`: `getBasePath() + ".idx"`
- `getMdPath()`: `getBasePath() + ".md"`
- `saveClipping()`:
  1. `SdMan.mkdir(CLIPPINGS_DIR)`
  2. Build the markdown text block for this clipping (chapter headings + page text, same format as current `PageExporter::writePassage`)
  3. Check if `.md` exists — if new, write YAML frontmatter header first
  4. Open `.md` in append mode, record `textOffset = file.size()` before writing, write the text block, record `textLength`
  5. Build `ClippingEntry` with the offset/length and metadata from `pages[0]`
  6. Load existing index entries, append new entry, write index
- `loadIndex()`: read magic+version+count header, then read `count * 16` bytes into entries
- `loadClippingText()`: open `.md`, seek to `entry.textOffset`, read `entry.textLength` bytes
- `loadClippingPreview()`: same but read only `min(maxChars, textLength)` bytes, strip leading whitespace/newlines, append `...` if truncated
- `deleteClipping()`:
  1. Load all entries, remove at index
  2. Rebuild `.md` by reading each remaining clipping's text and writing a new file
  3. Update offsets in entries, rewrite `.idx`

### Success Criteria:

#### Automated:
- [ ] Build passes: `pio run`
- [ ] No cppcheck warnings on new files

#### Manual:
- [ ] Capture creates both `.idx` and `.md` files in `/.crosspoint/clippings/`
- [ ] `.md` file is well-formatted markdown with YAML frontmatter
- [ ] Index correctly tracks offsets into the `.md` file

---

## Phase 2: Clippings List UI

### Overview
Create `EpubReaderClippingsListActivity` modeled after `EpubReaderBookmarkListActivity`, showing text previews and deletion hint.

### Changes Required:

#### 1. New file: `src/activities/reader/EpubReaderClippingsListActivity.h`

```cpp
#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "ClippingStore.h"

class EpubReaderClippingsListActivity final : public ActivityWithSubactivity {
  std::string bookPath;
  std::vector<ClippingEntry> clippings;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int selectorIndex = 0;
  bool updateRequired = false;
  bool confirmingDelete = false;

  const std::function<void()> onGoBack;

  int getPageItems() const;
  int getTotalItems() const;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();

 public:
  explicit EpubReaderClippingsListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& bookPath,
                                           const std::function<void()>& onGoBack)
      : ActivityWithSubactivity("EpubReaderClippingsList", renderer, mappedInput),
        bookPath(bookPath),
        onGoBack(onGoBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
```

#### 2. New file: `src/activities/reader/EpubReaderClippingsListActivity.cpp`

Key implementation details:
- `onEnter()`: load clippings via `ClippingStore::loadIndex(bookPath)`
- `renderScreen()`:
  - Title: `confirmingDelete ? "Delete clipping?" : "Clippings"`
  - Subtitle hint: `"Hold to delete"` (always shown below title, like @osteotek requested)
  - Empty state: `"No clippings"` with Back hint
  - For each visible clipping: load preview via `ClippingStore::loadClippingPreview(bookPath, entry, previewChars)` where `previewChars` is calculated from screen width
  - Display: `"preview text here..."` — just the text preview, truncated to fit one line
  - Footer hints: `"« Back" | "View" | "Up" | "Down"` (normal), `"Cancel" | "Delete"` (confirming)
- `loop()`:
  - Same navigation pattern as bookmark list (Up/Down with page skip on long press)
  - Short press Confirm: open `ClippingTextViewerActivity` as subactivity with the full text
  - Long press Confirm: enter delete confirmation mode
  - Back: `onGoBack()`

#### 3. Hint approach for "Hold to delete"

Draw a small hint line below the title in a lighter font:
```cpp
renderer.drawCenteredText(SMALL_FONT_ID, 40 + contentY, "Hold \xE2\x97\x8F to delete");
// or simpler:
renderer.drawCenteredText(SMALL_FONT_ID, 40 + contentY, "Hold to delete");
```

### Success Criteria:

#### Automated:
- [ ] Build passes: `pio run`

#### Manual:
- [ ] Clippings list shows text preview per entry, truncated with `...`
- [ ] "Hold to delete" hint visible below title
- [ ] Navigation works (Up/Down, page skip)
- [ ] Delete confirmation flow works
- [ ] Empty state shown when no clippings
- [ ] Back returns to reader

---

## Phase 3: Clipping Text Viewer

### Overview
Create `ClippingTextViewerActivity` — a simple scrollable text display for viewing the full text of a clipping.

### Changes Required:

#### 1. New file: `src/activities/reader/ClippingTextViewerActivity.h`

```cpp
#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"

class ClippingTextViewerActivity final : public ActivityWithSubactivity {
  std::string text;
  std::vector<std::string> lines;  // text split into screen-width lines
  int scrollOffset = 0;            // first visible line index
  int linesPerPage = 0;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;

  const std::function<void()> onGoBack;

  void wrapText();  // Split text into lines that fit screen width

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();

 public:
  explicit ClippingTextViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                      std::string text,
                                      const std::function<void()>& onGoBack)
      : ActivityWithSubactivity("ClippingTextViewer", renderer, mappedInput),
        text(std::move(text)),
        onGoBack(onGoBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
```

#### 2. New file: `src/activities/reader/ClippingTextViewerActivity.cpp`

Key implementation details:
- `onEnter()`: call `wrapText()` to split text into lines, calculate `linesPerPage`
- `wrapText()`: iterate through `text`, use `renderer.getTextWidth()` to find line break points. Split on word boundaries. Handle `\n` as forced line breaks.
- `renderScreen()`:
  - Clear screen
  - Draw lines from `scrollOffset` to `scrollOffset + linesPerPage`
  - Show scroll position indicator: `"Line X-Y of Z"` or a simple progress fraction in status area
  - Footer hints: `"« Back" | "" | "Up" | "Down"`
- `loop()`:
  - Up/Down: scroll by 1 line (short press) or 1 page (long press)
  - Back: `onGoBack()`
  - No Confirm action needed

### Success Criteria:

#### Automated:
- [ ] Build passes: `pio run`

#### Manual:
- [ ] Full clipping text visible and scrollable
- [ ] Word wrap works correctly
- [ ] Scroll position indicator shown
- [ ] Back returns to clippings list

---

## Phase 4: Integrate Capture Flow & Menu

### Overview
Wire everything together: update the capture flow to auto-save on menu open, update menu items, remove `onOpenFile` callback, add Clippings menu handler.

### Changes Required:

#### 1. `src/activities/reader/EpubReaderMenuActivity.h`

Update menu items and enum:
- Remove `VIEW_CLIPPINGS` from enum, rename to `CLIPPINGS`
- Rename `START_CAPTURE` to `CAPTURE`
- Menu label: `"Capture"` (not "Capture Clipping")
- Reorder: Clippings before Capture
- When `isCapturing`: no label change needed — opening menu auto-saves, so `isCapturing` will always be false when menu appears
- Remove `isCapturing` parameter from constructor entirely

```cpp
enum class MenuAction {
  SELECT_CHAPTER,
  BOOKMARKS,
  CLIPPINGS,
  CAPTURE,
  GO_TO_PERCENT,
  ROTATE_SCREEN,
  GO_HOME,
  SYNC,
  DELETE_CACHE
};

// Menu items:
std::vector<MenuItem> menuItems = {
  {MenuAction::SELECT_CHAPTER, "Go to Chapter"},
  {MenuAction::BOOKMARKS, "Bookmarks"},
  {MenuAction::CLIPPINGS, "Clippings"},
  {MenuAction::CAPTURE, "Capture"},
  {MenuAction::ROTATE_SCREEN, "Reading Orientation"},
  {MenuAction::GO_TO_PERCENT, "Go to %"},
  {MenuAction::GO_HOME, "Go Home"},
  {MenuAction::SYNC, "Sync Progress"},
  {MenuAction::DELETE_CACHE, "Delete Book Cache"}
};
```

Constructor no longer takes `isCapturing` and no longer mutates menu items.

#### 2. `src/activities/reader/EpubReaderActivity.h`

- Remove `#include "PageExporter.h"`, add `#include "ClippingStore.h"`
- Remove `const std::function<void(const std::string&)> onOpenFile`
- Remove `std::string pendingOpenFilePath`
- Change `statusBarMarker` behavior: instead of `*`, show `"Capturing..."` via `statusBarOverride`
- Remove `statusBarMarker` bool entirely — use `captureState == CAPTURING` check in status bar
- Keep `captureBuffer` but change type to `std::vector<CapturedPage>` (from new `ClippingStore.h`)
- Update constructor to remove `onOpenFile` parameter

```cpp
explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                            std::unique_ptr<Epub> epub,
                            const std::function<void()>& onGoBack,
                            const std::function<void()>& onGoHome)
    : ActivityWithSubactivity("EpubReader", renderer, mappedInput),
      epub(std::move(epub)),
      onGoBack(onGoBack),
      onGoHome(onGoHome) {}
```

#### 3. `src/activities/reader/EpubReaderActivity.cpp`

**Auto-save on menu open** — in the Confirm short-press handler (line ~303):
```cpp
// Short press CONFIRM opens reader menu
if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() < captureHoldMs) {
  // Auto-save capture when opening menu
  if (captureState == CaptureState::CAPTURING) {
    stopCapture();  // saves to ClippingStore, sets statusBarOverride = "Clipping saved"
  }
  // ... rest of menu opening code (without isCapturing parameter)
```

**Status bar "Capturing..."** — in `startCapture()`:
```cpp
void EpubReaderActivity::startCapture() {
  if (epub && epub->getPath().find("/.crosspoint/") != std::string::npos) {
    statusBarOverride = "Cannot capture here";
    updateRequired = true;
    return;
  }
  captureBuffer.clear();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  captureCurrentPage();
  xSemaphoreGive(renderingMutex);
  captureState = CaptureState::CAPTURING;
  statusBarOverride = "Capturing...";
  updateRequired = true;
}
```

**Persistent "Capturing..." in status bar** — modify `renderStatusBar()`:
- Remove all `statusBarMarker` references
- After the `statusBarOverride` block, add a check for capture state:
```cpp
if (captureState == CaptureState::CAPTURING && statusBarOverride.empty()) {
  // Show persistent "Capturing..." indicator
  const auto screenHeight = renderer.getScreenHeight();
  const auto textY = screenHeight - orientedMarginBottom - 4;
  const char* capText = "Capturing...";
  const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, capText);
  const int x = (renderer.getScreenWidth() - textWidth) / 2;
  renderer.drawText(SMALL_FONT_ID, x, textY, capText);
  return;
}
```

**Update `stopCapture()`** to use `ClippingStore`:
```cpp
void EpubReaderActivity::stopCapture() {
  if (captureBuffer.empty()) {
    cancelCapture();
    return;
  }
  const bool ok = ClippingStore::saveClipping(epub->getPath(), epub->getTitle(), epub->getAuthor(), captureBuffer);
  statusBarOverride = ok ? "Clipping saved" : "Save failed";
  captureBuffer.clear();
  captureState = CaptureState::IDLE;
  updateRequired = true;
}
```

**Update `captureCurrentPage()`** to include spineIndex and pageIndex in CapturedPage:
```cpp
captureBuffer.push_back({pageText, chapterTitle, bookPercent, chapterPercent, currentSpineIndex, section->currentPage});
```

**Remove pendingOpenFilePath handler** from `loop()` (lines 264-278).

**Update menu confirm handler:**
- Remove `VIEW_CLIPPINGS` case
- Add `CLIPPINGS` case:
```cpp
case EpubReaderMenuActivity::MenuAction::CLIPPINGS: {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  exitActivity();
  enterNewActivity(new EpubReaderClippingsListActivity(
      this->renderer, this->mappedInput, epub->getPath(),
      [this]() {
        exitActivity();
        updateRequired = true;
      }));
  xSemaphoreGive(renderingMutex);
  break;
}
```

- Update `CAPTURE` case (was `START_CAPTURE`):
```cpp
case EpubReaderMenuActivity::MenuAction::CAPTURE: {
  exitActivity();
  applyOrientation(SETTINGS.orientation);
  startCapture();  // Always starts (menu auto-saved if was capturing)
  break;
}
```

**Update path guard in `addBookmark()`** — change `/clippings/` check to `/.crosspoint/`:
```cpp
if (epub->getPath().find("/.crosspoint/") != std::string::npos) {
```

**Clear statusBarOverride on page turn** — already handled (line 357), but also don't clear it when it says "Capturing..." — actually no, the persistent capture indicator is now driven by `captureState` not `statusBarOverride`, so clearing is fine. `statusBarOverride` is only for transient messages like "Clipping saved".

Actually wait — we need `statusBarOverride` to NOT persist "Capturing..." since we're using the capture state check instead. In `startCapture()`, we should NOT set `statusBarOverride = "Capturing..."` — that would get cleared on page turn. Instead, the persistent indicator comes from the `captureState == CAPTURING` check in `renderStatusBar()`. So `startCapture()` should just set `statusBarOverride = "Capture started"` as a transient confirmation that clears on first page turn.

Correction to `startCapture()`:
```cpp
statusBarOverride = "Capture started";
```

#### 4. `src/activities/reader/EpubReaderMenuActivity.h` (constructor)

Remove `isCapturing` parameter and the menu item mutation logic:
```cpp
explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                const std::string& title, const int currentPage,
                                const int totalPages, const int bookProgressPercent,
                                const uint8_t currentOrientation,
                                const std::function<void(uint8_t)>& onBack,
                                const std::function<void(MenuAction)>& onAction)
```

#### 5. `src/activities/reader/ReaderActivity.h` and `ReaderActivity.cpp`

- Remove `void openFile(const std::string& filePath)` method
- Update `onGoToEpubReader()` to not pass `onOpenFile`:
```cpp
void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  exitActivity();
  enterNewActivity(new EpubReaderActivity(
      renderer, mappedInput, std::move(epub),
      [this, epubPath] { goToLibrary(epubPath); },
      [this] { onGoBack(); }));
}
```

#### 6. Delete `src/PageExporter.h` and `src/PageExporter.cpp`

No longer needed — replaced by `ClippingStore`.

### Success Criteria:

#### Automated:
- [ ] Build passes: `pio run`
- [ ] clang-format passes

#### Manual:
- [ ] Starting capture shows "Capture started" briefly, then "Capturing..." persists
- [ ] "Capturing..." stays on status bar across page turns
- [ ] Opening menu while capturing auto-saves and shows "Clipping saved"
- [ ] Menu no longer has "Stop Capture" — always shows "Capture"
- [ ] Clippings menu item opens the clippings list
- [ ] Back from clippings list returns to reading at exact same position

---

## Phase 5: Update USER_GUIDE.md

### Changes:
- Add "Clippings" section after Bookmarks
- Document capture flow: Menu → Capture → page turns → Menu to save
- Document clippings viewer: Menu → Clippings → browse/view/delete
- Mention "Hold to delete" interaction
- Remove any old "Save Passage" or "/clippings" folder references

---

## Phase 6: Format, Build, Flash

1. Run clang-format: `/opt/homebrew/opt/llvm/bin/clang-format -i` on all new/modified files
2. Build: `pio run`
3. Flash: `pio run --target upload --upload-port /dev/cu.usbmodem2101`

### Success Criteria:

#### Automated:
- [ ] clang-format produces no changes
- [ ] `pio run` builds successfully
- [ ] Device flashes successfully

#### Manual:
- [ ] Full end-to-end test: capture clipping → view in list → view full text → delete → verify

---

## Files Summary

| File | Phase | Action |
|------|-------|--------|
| `src/ClippingStore.h` | 1 | NEW |
| `src/ClippingStore.cpp` | 1 | NEW |
| `src/activities/reader/EpubReaderClippingsListActivity.h` | 2 | NEW |
| `src/activities/reader/EpubReaderClippingsListActivity.cpp` | 2 | NEW |
| `src/activities/reader/ClippingTextViewerActivity.h` | 3 | NEW |
| `src/activities/reader/ClippingTextViewerActivity.cpp` | 3 | NEW |
| `src/activities/reader/EpubReaderMenuActivity.h` | 4 | MODIFY |
| `src/activities/reader/EpubReaderActivity.h` | 4 | MODIFY |
| `src/activities/reader/EpubReaderActivity.cpp` | 4 | MODIFY |
| `src/activities/reader/ReaderActivity.h` | 4 | MODIFY |
| `src/activities/reader/ReaderActivity.cpp` | 4 | MODIFY |
| `src/PageExporter.h` | 4 | DELETE |
| `src/PageExporter.cpp` | 4 | DELETE |
| `USER_GUIDE.md` | 5 | MODIFY |

## Key Design Decisions

1. **Two-file storage (`.idx` + `.md`)**: The `.idx` enables fast list loading without parsing markdown. The `.md` provides human-readable export. Both live in `/.crosspoint/clippings/` to stay hidden from the device file browser.

2. **Preview loading**: `loadClippingPreview()` reads only the first N bytes from `.md` using the offset from `.idx`. This avoids loading full text into RAM for the list view.

3. **Auto-save on menu open**: Simplifies UX — no "Stop Capture" action needed. Opening the menu is a natural stopping point. The `isCapturing` parameter is removed from `EpubReaderMenuActivity` entirely.

4. **Persistent "Capturing..." indicator**: Driven by `captureState == CAPTURING` check in `renderStatusBar()`, not by `statusBarOverride`. This means it survives page turns (which clear `statusBarOverride`).

5. **Subactivity stack for position preservation**: `EpubReaderActivity → ClippingsList → TextViewer`. The reader's state is completely untouched throughout — no save/restore needed.

6. **Delete rewrites both files**: Since `.md` text offsets would become invalid after deletion, we rebuild both files. This is acceptable since deletes are rare and clipping files are small.
