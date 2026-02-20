# Bookmark Navigation Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Separate bookmarks from saved passages — store bookmarks in binary files, add a bookmark list browser screen, make long-press Confirm create bookmarks directly, and keep Save Passage as a menu-only action.

**Architecture:** Remove the popup menu. Long-press Confirm writes a bookmark directly to a binary file at `/Bookmarks/<book-filename>.bookmarks`. A new `BookmarkStore` utility handles binary I/O. A new `EpubReaderBookmarkListActivity` subactivity (modeled after `EpubReaderChapterSelectionActivity`) lets users browse, delete, and jump to bookmarks. Menu items change: "Bookmark & Save" → "Save Passage" (direct capture start), new "Bookmarks" entry opens the bookmark list.

**Tech Stack:** C++ (Arduino/ESP32), PlatformIO, FreeRTOS, SdFat (SD card I/O), GfxRenderer (e-ink drawing)

---

## Files Overview

| File | Action |
|------|--------|
| `src/BookmarkStore.h` | Create — bookmark binary file I/O utility |
| `src/BookmarkStore.cpp` | Create — implementation |
| `src/activities/reader/EpubReaderBookmarkListActivity.h` | Create — bookmark list browser screen |
| `src/activities/reader/EpubReaderBookmarkListActivity.cpp` | Create — implementation |
| `src/activities/reader/EpubReaderActivity.h` | Modify — remove popup state, add bookmark helpers |
| `src/activities/reader/EpubReaderActivity.cpp` | Modify — remove popup code, wire bookmark + bookmark list |
| `src/activities/reader/EpubReaderMenuActivity.h` | Modify — rename menu item, add BOOKMARKS action |
| `USER_GUIDE.md` | Modify — update Bookmarks & Saved Passages section |

---

## Task 1: Create BookmarkStore utility

**Files:**
- Create: `src/BookmarkStore.h`
- Create: `src/BookmarkStore.cpp`

### Step 1: Create the header

Write `src/BookmarkStore.h`:

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A single bookmark entry — a position in a book.
struct BookmarkEntry {
  uint8_t bookPercent;   // 0-100 overall book progress
  uint16_t spineIndex;   // Spine item index
  uint16_t pageIndex;    // Page index within spine item
};

// Stores and retrieves bookmarks in binary files on the SD card.
// Files are stored at /Bookmarks/<book-filename>.bookmarks
// Binary format: [version:1][count:1][entries: count * 5 bytes]
// Each entry: [bookPercent:1][spineIndex:2 LE][pageIndex:2 LE]
class BookmarkStore {
 public:
  // Add a bookmark. Skips if a bookmark with the same bookPercent already exists.
  // Returns true if the bookmark was added (or already existed).
  static bool addBookmark(const std::string& bookPath, const BookmarkEntry& entry);

  // Load all bookmarks for a book, sorted by bookPercent ascending.
  static std::vector<BookmarkEntry> loadBookmarks(const std::string& bookPath);

  // Delete a bookmark at the given index. Returns true on success.
  static bool deleteBookmark(const std::string& bookPath, int index);

 private:
  static std::string getBookmarkPath(const std::string& bookPath);
  static bool writeBookmarks(const std::string& path, const std::vector<BookmarkEntry>& entries);
  static constexpr uint8_t FORMAT_VERSION = 1;
  static constexpr const char* BOOKMARKS_DIR = "/Bookmarks";
  static constexpr const char* TAG = "BKM";
};
```

### Step 2: Create the implementation

Write `src/BookmarkStore.cpp`:

```cpp
#include "BookmarkStore.h"

#include <SDCardManager.h>

#include <algorithm>

std::string BookmarkStore::getBookmarkPath(const std::string& bookPath) {
  std::string filename;
  const auto lastSlash = bookPath.rfind('/');
  if (lastSlash != std::string::npos) {
    filename = bookPath.substr(lastSlash + 1);
  } else {
    filename = bookPath;
  }
  const auto lastDot = filename.rfind('.');
  if (lastDot != std::string::npos) {
    filename.resize(lastDot);
  }
  if (filename.empty()) {
    filename = "untitled";
  }
  return std::string(BOOKMARKS_DIR) + "/" + filename + ".bookmarks";
}

std::vector<BookmarkEntry> BookmarkStore::loadBookmarks(const std::string& bookPath) {
  std::vector<BookmarkEntry> entries;
  const std::string path = getBookmarkPath(bookPath);

  FsFile file;
  if (!SdMan.openFileForRead(TAG, path, file)) {
    return entries;
  }

  uint8_t header[2];
  if (file.read(header, 2) != 2 || header[0] != FORMAT_VERSION) {
    file.close();
    return entries;
  }

  const uint8_t count = header[1];
  for (uint8_t i = 0; i < count; i++) {
    uint8_t data[5];
    if (file.read(data, 5) != 5) {
      break;
    }
    BookmarkEntry entry;
    entry.bookPercent = data[0];
    entry.spineIndex = data[1] | (data[2] << 8);
    entry.pageIndex = data[3] | (data[4] << 8);
    entries.push_back(entry);
  }

  file.close();
  return entries;
}

bool BookmarkStore::writeBookmarks(const std::string& path, const std::vector<BookmarkEntry>& entries) {
  FsFile file = SdMan.open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    Serial.printf("[%lu] [%s] Failed to open bookmark file: %s\n", millis(), TAG, path.c_str());
    return false;
  }

  uint8_t header[2] = {FORMAT_VERSION, static_cast<uint8_t>(entries.size())};
  if (file.write(header, 2) != 2) {
    file.close();
    return false;
  }

  for (const auto& entry : entries) {
    uint8_t data[5];
    data[0] = entry.bookPercent;
    data[1] = entry.spineIndex & 0xFF;
    data[2] = (entry.spineIndex >> 8) & 0xFF;
    data[3] = entry.pageIndex & 0xFF;
    data[4] = (entry.pageIndex >> 8) & 0xFF;
    if (file.write(data, 5) != 5) {
      file.close();
      return false;
    }
  }

  file.close();
  return true;
}

bool BookmarkStore::addBookmark(const std::string& bookPath, const BookmarkEntry& entry) {
  SdMan.mkdir(BOOKMARKS_DIR);
  const std::string path = getBookmarkPath(bookPath);

  auto entries = loadBookmarks(bookPath);

  // Skip duplicate (same bookPercent)
  for (const auto& existing : entries) {
    if (existing.bookPercent == entry.bookPercent) {
      Serial.printf("[%lu] [%s] Bookmark at %d%% already exists\n", millis(), TAG, entry.bookPercent);
      return true;
    }
  }

  entries.push_back(entry);

  // Sort by bookPercent ascending
  std::sort(entries.begin(), entries.end(),
            [](const BookmarkEntry& a, const BookmarkEntry& b) { return a.bookPercent < b.bookPercent; });

  // Enforce max 255 entries (uint8_t count)
  if (entries.size() > 255) {
    entries.resize(255);
  }

  const bool ok = writeBookmarks(path, entries);
  if (ok) {
    Serial.printf("[%lu] [%s] Bookmark added at %d%% (total: %d)\n", millis(), TAG, entry.bookPercent,
                  static_cast<int>(entries.size()));
  }
  return ok;
}

bool BookmarkStore::deleteBookmark(const std::string& bookPath, int index) {
  const std::string path = getBookmarkPath(bookPath);
  auto entries = loadBookmarks(bookPath);

  if (index < 0 || index >= static_cast<int>(entries.size())) {
    return false;
  }

  entries.erase(entries.begin() + index);
  return writeBookmarks(path, entries);
}
```

### Step 3: Verify build

Run: `cd /Users/tobi/dotfiles/crosspoint-reader && pio run 2>&1 | tail -5`
Expected: Compiles without errors (BookmarkStore is not yet referenced, but PlatformIO should pick it up from `src/`).

### Step 4: Commit

```bash
git add src/BookmarkStore.h src/BookmarkStore.cpp
git commit -m "feat: add BookmarkStore binary file utility"
```

---

## Task 2: Remove popup menu code from EpubReaderActivity

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.h`
- Modify: `src/activities/reader/EpubReaderActivity.cpp`

This task removes all popup-related code. It replaces long-press Confirm (IDLE) with a direct bookmark write, and changes the menu handler to start capture directly.

### Step 1: Update header — remove popup state and add bookmark method

In `src/activities/reader/EpubReaderActivity.h`:

1. Add `#include "BookmarkStore.h"` after the existing `#include "PageExporter.h"` (line 13).

2. Replace the CaptureState enum (line 38):
   ```cpp
   // Old:
   enum class CaptureState { IDLE, POPUP_MENU, CAPTURING };
   // New:
   enum class CaptureState { IDLE, CAPTURING };
   ```

3. Remove `popupSelectedIndex` (line 42):
   ```cpp
   // DELETE: int popupSelectedIndex = 0;              // 0 = Bookmark, 1 = Save Passage
   ```

4. Remove `pendingPopupMenu` (line 35):
   ```cpp
   // DELETE: bool pendingPopupMenu = false;        // Deferred popup from menu
   ```

5. In the capture helpers section (lines 61-67), remove `writeBookmark()` and `renderPopupMenu()`, add `addBookmark()`:
   ```cpp
   // Capture helpers
   void captureCurrentPage();
   void startCapture();
   void stopCapture();
   void cancelCapture();
   void addBookmark();
   ```

### Step 2: Update implementation — remove popup, wire direct bookmark

In `src/activities/reader/EpubReaderActivity.cpp`:

1. **Delete `renderPopupMenu()`** (lines 200-235) — entire function.

2. **Replace `writeBookmark()`** (lines 237-262) with `addBookmark()`:
   ```cpp
   void EpubReaderActivity::addBookmark() {
     if (!section || !epub) {
       return;
     }
     // Prevent bookmarking from books stored inside the exports directory
     if (epub->getPath().find("/Saved Passages/") != std::string::npos) {
       statusBarOverride = "Cannot bookmark here";
       updateRequired = true;
       return;
     }
     xSemaphoreTake(renderingMutex, portMAX_DELAY);
     const float chapterProgress = (section->pageCount > 0)
                                       ? static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount)
                                       : 0.0f;
     const int bookPercent =
         clampPercent(static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f));
     xSemaphoreGive(renderingMutex);

     BookmarkEntry entry;
     entry.bookPercent = static_cast<uint8_t>(bookPercent);
     entry.spineIndex = static_cast<uint16_t>(currentSpineIndex);
     entry.pageIndex = section ? static_cast<uint16_t>(section->currentPage) : 0;

     const bool ok = BookmarkStore::addBookmark(epub->getPath(), entry);
     statusBarOverride = ok ? "Bookmarked" : "Bookmark failed";
     updateRequired = true;
   }
   ```

3. **Delete the `pendingPopupMenu` handler** (lines 296-305):
   ```cpp
   // DELETE this entire block:
   // Handle deferred popup from menu
   if (pendingPopupMenu) { ... }
   ```

4. **Delete the entire POPUP_MENU input block** (lines 321-351):
   ```cpp
   // DELETE this entire block:
   // Popup menu input handling — intercepts all input while popup is visible
   if (captureState == CaptureState::POPUP_MENU) { ... }
   ```

5. **Replace long-press Confirm handler** (lines 353-367). Change the IDLE branch from showing popup to calling `addBookmark()`:
   ```cpp
   // Long press CONFIRM (1s+): bookmark (IDLE) or stop capture (CAPTURING)
   if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= captureHoldMs) {
     if (captureState == CaptureState::IDLE) {
       addBookmark();
     } else if (captureState == CaptureState::CAPTURING) {
       stopCapture();
     }
     // Wait for button release before processing further input
     skipNextButtonCheck = true;
     return;
   }
   ```

6. **Update `onReaderMenuConfirm(START_CAPTURE)`** (lines 615-623). Replace popup trigger with direct capture start:
   ```cpp
   case EpubReaderMenuActivity::MenuAction::START_CAPTURE: {
     exitActivity();
     applyOrientation(SETTINGS.orientation);
     if (captureState == CaptureState::CAPTURING) {
       stopCapture();
     } else {
       startCapture();
     }
     break;
   }
   ```

7. **Remove popup rendering in `renderContents()`** (lines 904-906):
   ```cpp
   // DELETE:
   if (captureState == CaptureState::POPUP_MENU) {
     renderPopupMenu();
   }
   ```

8. **Update `EpubReaderMenuActivity` constructor call** (line 384). Change `captureState == CaptureState::CAPTURING` — this still works since `POPUP_MENU` is removed.

### Step 3: Verify build

Run: `cd /Users/tobi/dotfiles/crosspoint-reader && pio run 2>&1 | tail -5`
Expected: Compiles without errors.

### Step 4: Run formatter

Run: `cd /Users/tobi/dotfiles/crosspoint-reader && /opt/homebrew/opt/llvm/bin/clang-format -i src/activities/reader/EpubReaderActivity.h src/activities/reader/EpubReaderActivity.cpp`

### Step 5: Commit

```bash
git add src/activities/reader/EpubReaderActivity.h src/activities/reader/EpubReaderActivity.cpp
git commit -m "refactor: replace popup menu with direct bookmark via BookmarkStore"
```

---

## Task 3: Update reader menu items

**Files:**
- Modify: `src/activities/reader/EpubReaderMenuActivity.h`

### Step 1: Add BOOKMARKS action and rename menu item

In `src/activities/reader/EpubReaderMenuActivity.h`:

1. Add `BOOKMARKS` to the `MenuAction` enum (line 17):
   ```cpp
   enum class MenuAction { SELECT_CHAPTER, START_CAPTURE, BOOKMARKS, GO_TO_PERCENT, ROTATE_SCREEN, GO_HOME, SYNC, DELETE_CACHE };
   ```

2. Update `menuItems` (lines 52-58):
   ```cpp
   std::vector<MenuItem> menuItems = {{MenuAction::SELECT_CHAPTER, "Go to Chapter"},
                                      {MenuAction::START_CAPTURE, "Save Passage"},
                                      {MenuAction::BOOKMARKS, "Bookmarks"},
                                      {MenuAction::ROTATE_SCREEN, "Reading Orientation"},
                                      {MenuAction::GO_TO_PERCENT, "Go to %"},
                                      {MenuAction::GO_HOME, "Go Home"},
                                      {MenuAction::SYNC, "Sync Progress"},
                                      {MenuAction::DELETE_CACHE, "Delete Book Cache"}};
   ```

   Note: "Bookmark & Save" → "Save Passage", and a new "Bookmarks" entry is added.

### Step 2: Verify build

Run: `cd /Users/tobi/dotfiles/crosspoint-reader && pio run 2>&1 | tail -5`
Expected: Compile error in `onReaderMenuConfirm()` because `BOOKMARKS` case is not handled. This is expected — it will be wired in the next task.

### Step 3: Run formatter

Run: `cd /Users/tobi/dotfiles/crosspoint-reader && /opt/homebrew/opt/llvm/bin/clang-format -i src/activities/reader/EpubReaderMenuActivity.h`

### Step 4: Commit

```bash
git add src/activities/reader/EpubReaderMenuActivity.h
git commit -m "feat: add Bookmarks menu item, rename to Save Passage"
```

---

## Task 4: Create EpubReaderBookmarkListActivity

**Files:**
- Create: `src/activities/reader/EpubReaderBookmarkListActivity.h`
- Create: `src/activities/reader/EpubReaderBookmarkListActivity.cpp`

Model this closely after `EpubReaderChapterSelectionActivity`. The bookmark list shows entries like `42% — Spine 5, Page 12`. Navigation uses Vol Up/Down (or Left/Right), Left button deletes (with confirmation), Right/Confirm jumps to the bookmark.

### Step 1: Create the header

Write `src/activities/reader/EpubReaderBookmarkListActivity.h`:

```cpp
#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "BookmarkStore.h"

class EpubReaderBookmarkListActivity final : public ActivityWithSubactivity {
  std::string bookPath;
  std::vector<BookmarkEntry> bookmarks;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int selectorIndex = 0;
  bool updateRequired = false;
  bool confirmingDelete = false;

  const std::function<void()> onGoBack;
  // Callback: jump to spine index + page index
  const std::function<void(int spineIndex, int pageIndex)> onSelectBookmark;

  int getPageItems() const;
  int getTotalItems() const;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();

 public:
  explicit EpubReaderBookmarkListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                          const std::string& bookPath, const std::function<void()>& onGoBack,
                                          const std::function<void(int spineIndex, int pageIndex)>& onSelectBookmark)
      : ActivityWithSubactivity("EpubReaderBookmarkList", renderer, mappedInput),
        bookPath(bookPath),
        onGoBack(onGoBack),
        onSelectBookmark(onSelectBookmark) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
```

### Step 2: Create the implementation

Write `src/activities/reader/EpubReaderBookmarkListActivity.cpp`:

```cpp
#include "EpubReaderBookmarkListActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
}  // namespace

int EpubReaderBookmarkListActivity::getTotalItems() const { return static_cast<int>(bookmarks.size()); }

int EpubReaderBookmarkListActivity::getPageItems() const {
  constexpr int lineHeight = 30;
  const int screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = 60 + hintGutterHeight;
  const int availableHeight = screenHeight - startY - lineHeight;
  return std::max(1, availableHeight / lineHeight);
}

void EpubReaderBookmarkListActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderBookmarkListActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderBookmarkListActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  bookmarks = BookmarkStore::loadBookmarks(bookPath);
  renderingMutex = xSemaphoreCreateMutex();

  // Clamp selector to valid range
  if (selectorIndex >= getTotalItems()) {
    selectorIndex = std::max(0, getTotalItems() - 1);
  }

  updateRequired = true;
  xTaskCreate(&EpubReaderBookmarkListActivity::taskTrampoline, "BookmarkListTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void EpubReaderBookmarkListActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubReaderBookmarkListActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  const int totalItems = getTotalItems();

  // Handle empty bookmark list
  if (totalItems == 0 && !confirmingDelete) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      onGoBack();
    }
    return;
  }

  // Delete confirmation mode
  if (confirmingDelete) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Confirm delete
      BookmarkStore::deleteBookmark(bookPath, selectorIndex);
      bookmarks = BookmarkStore::loadBookmarks(bookPath);
      if (selectorIndex >= getTotalItems()) {
        selectorIndex = std::max(0, getTotalItems() - 1);
      }
      confirmingDelete = false;
      updateRequired = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      // Cancel delete
      confirmingDelete = false;
      updateRequired = true;
    }
    return;
  }

  // Normal navigation
  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);
  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = getPageItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Jump to bookmark
    if (selectorIndex >= 0 && selectorIndex < totalItems) {
      const auto& bk = bookmarks[selectorIndex];
      onSelectBookmark(bk.spineIndex, bk.pageIndex);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Check for long press = delete
    if (mappedInput.getHeldTime() > SKIP_PAGE_MS) {
      confirmingDelete = true;
      updateRequired = true;
    } else {
      onGoBack();
    }
  } else if (prevReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems - 1) * pageItems + totalItems) % totalItems;
    } else {
      selectorIndex = (selectorIndex + totalItems - 1) % totalItems;
    }
    updateRequired = true;
  } else if (nextReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % totalItems;
    } else {
      selectorIndex = (selectorIndex + 1) % totalItems;
    }
    updateRequired = true;
  }
}

void EpubReaderBookmarkListActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void EpubReaderBookmarkListActivity::renderScreen() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;
  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

  // Title
  const char* titleText = confirmingDelete ? "Delete bookmark?" : "Bookmarks";
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, titleText, EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, titleText, true, EpdFontFamily::BOLD);

  if (totalItems == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "No bookmarks", true);
    const auto labels = mappedInput.mapLabels("« Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  // Highlight selected row
  renderer.fillRect(contentX, 60 + contentY + (selectorIndex % pageItems) * 30 - 2, contentWidth - 1, 30);

  for (int i = 0; i < pageItems; i++) {
    const int itemIndex = pageStartIndex + i;
    if (itemIndex >= totalItems) break;
    const int displayY = 60 + contentY + i * 30;
    const bool isSelected = (itemIndex == selectorIndex);

    const auto& bk = bookmarks[itemIndex];
    char label[48];
    snprintf(label, sizeof(label), "%d%% of book", bk.bookPercent);

    const int textX = contentX + 20;
    renderer.drawText(UI_10_FONT_ID, textX, displayY, label, !isSelected);
  }

  if (confirmingDelete) {
    const auto labels = mappedInput.mapLabels("Cancel", "Delete", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels("« Back", "Go to", "Up", "Down");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
```

**Design notes:**
- Navigation: Vol Up/Down (Left/Right) to move cursor
- Confirm: Jump to the selected bookmark position
- Long-press Back: Enter delete confirmation mode (title changes to "Delete bookmark?", button hints change)
- In delete confirmation: Confirm = delete, Back = cancel
- Short-press Back: Go back to reader
- Empty list: Shows "No bookmarks" with just a Back button hint

### Step 3: Verify build

Run: `cd /Users/tobi/dotfiles/crosspoint-reader && pio run 2>&1 | tail -5`
Expected: Compiles (files are not yet referenced from EpubReaderActivity).

### Step 4: Run formatter

Run: `cd /Users/tobi/dotfiles/crosspoint-reader && /opt/homebrew/opt/llvm/bin/clang-format -i src/activities/reader/EpubReaderBookmarkListActivity.h src/activities/reader/EpubReaderBookmarkListActivity.cpp`

### Step 5: Commit

```bash
git add src/activities/reader/EpubReaderBookmarkListActivity.h src/activities/reader/EpubReaderBookmarkListActivity.cpp
git commit -m "feat: add EpubReaderBookmarkListActivity for browsing bookmarks"
```

---

## Task 5: Wire bookmark list into EpubReaderActivity

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.cpp`

### Step 1: Add include and handle BOOKMARKS menu action

In `src/activities/reader/EpubReaderActivity.cpp`:

1. Add `#include "EpubReaderBookmarkListActivity.h"` at the top (after `#include "EpubReaderChapterSelectionActivity.h"`, around line 10).

2. Add a new case in `onReaderMenuConfirm()` for `BOOKMARKS` (add before the `GO_TO_PERCENT` case):

   ```cpp
   case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
     xSemaphoreTake(renderingMutex, portMAX_DELAY);
     exitActivity();
     enterNewActivity(new EpubReaderBookmarkListActivity(
         this->renderer, this->mappedInput, epub->getPath(),
         [this]() {
           exitActivity();
           updateRequired = true;
         },
         [this](const int spineIndex, const int pageIndex) {
           if (currentSpineIndex != spineIndex || (section && section->currentPage != pageIndex)) {
             currentSpineIndex = spineIndex;
             nextPageNumber = pageIndex;
             section.reset();
           }
           exitActivity();
           updateRequired = true;
         }));
     xSemaphoreGive(renderingMutex);
     break;
   }
   ```

   This follows the same pattern as `SELECT_CHAPTER` — the onGoBack callback closes the subactivity and refreshes, the onSelectBookmark callback updates position and refreshes.

### Step 2: Verify build

Run: `cd /Users/tobi/dotfiles/crosspoint-reader && pio run 2>&1 | tail -5`
Expected: Compiles without errors. All menu actions now have handlers.

### Step 3: Run formatter

Run: `cd /Users/tobi/dotfiles/crosspoint-reader && /opt/homebrew/opt/llvm/bin/clang-format -i src/activities/reader/EpubReaderActivity.cpp`

### Step 4: Commit

```bash
git add src/activities/reader/EpubReaderActivity.cpp
git commit -m "feat: wire bookmark list into reader menu"
```

---

## Task 6: Update USER_GUIDE.md

**File:** `USER_GUIDE.md`

### Step 1: Update Bookmarks & Saved Passages section

Replace the current "Bookmarks & Saved Passages" section (lines 202-229) with:

```markdown
### Bookmarks & Saved Passages

You can bookmark pages and save passages to a file on the SD card.

**Bookmarking or saving a passage:**
1. Hold **Confirm** for about 1 second — a popup appears with two options
2. Use **Left**/**Right** to select **Bookmark** or **Save Passage**
3. Press **Confirm** to execute your choice

**Bookmark:** Records the current page location in the book's annotation file.

**Save Passage (single page):** Starts a capture, then hold **Confirm** again to save just the current page.

**Save Passage (multi-page):**
1. Select **Save Passage** from the popup
2. Turn pages forward — each page is captured (indicated by **\*** in the status bar)
3. Hold **Confirm** again on the last page to save all captured pages

**Stopping a capture:**
- **Hold Confirm** to save the captured pages
- **Turn backward** to save and go back
- **Press Back** to discard the capture without saving
- **Open the reader menu** and select **Stop Capture** to save

> [!TIP]
> You can also access this from the reader menu: press **Confirm** → select **Bookmark & Save**.

Bookmarks and saved passages are stored in the **Saved Passages** folder on the SD card as `.md` files (one file per book). You can browse them directly on the device or on a computer.
```

Replace the above with the new separated content:

```markdown
### Bookmarks & Saved Passages

**Bookmarks** and **Saved Passages** are separate features for marking your place and saving text.

#### Bookmarks

Hold **Confirm** for about 1 second to bookmark the current page. The status bar will briefly show "Bookmarked".

**Viewing bookmarks:** Press **Confirm** to open the reader menu → select **Bookmarks**. This opens a list of all bookmarks for the current book, showing the book percentage for each.

**In the bookmark list:**
- Use **Up**/**Down** (or **Left**/**Right**) to navigate
- Press **Confirm** to jump to a bookmark
- Long-press **Back** to delete the selected bookmark (with confirmation)
- Press **Back** to return to reading

Bookmarks are stored in the **Bookmarks** folder on the SD card (one file per book).

#### Saved Passages

You can save text from pages to a file on the SD card.

**Quick save (single page):**
1. Press **Confirm** → select **Save Passage** from the reader menu
2. "Capture started" appears. Hold **Confirm** again — "Passage saved" appears

**Multi-page save:**
1. Press **Confirm** → select **Save Passage** from the reader menu
2. Turn pages forward — each page is captured (indicated by **\*** in the status bar)
3. Hold **Confirm** on the last page to save all captured pages

**Stopping a capture:**
- **Hold Confirm** to save the captured pages
- **Turn backward** to save and go back
- **Press Back** to discard the capture without saving
- **Open the reader menu** and select **Stop Capture** to save

Saved passages are stored in the **Saved Passages** folder on the SD card as `.md` files (one file per book). You can browse them directly on the device or on a computer.
```

### Step 2: Update the TOC entry (line 22)

The existing TOC entry `[Bookmarks & Saved Passages](#bookmarks--saved-passages)` is still valid since the section heading hasn't changed.

### Step 3: Update System Navigation line (line 234)

Change:
```markdown
* **Reader Menu:** Press **Confirm** to open the reader menu (chapter selection, bookmark & save, and more).
```
To:
```markdown
* **Reader Menu:** Press **Confirm** to open the reader menu (chapter selection, bookmarks, save passage, and more).
```

### Step 4: Commit

```bash
git add USER_GUIDE.md
git commit -m "docs: update user guide for separated bookmarks and passages"
```

---

## Task 7: Write design doc

**File:** Create `docs/plans/2026-02-06-bookmark-navigation-design.md`

Write a design doc summarizing the bookmark navigation feature. Key points to cover:
- Separation of bookmarks (binary, position-based) from passages (text-based, .md)
- Binary format: version byte, count byte, 5-byte entries
- Storage at `/Bookmarks/<book-filename>.bookmarks`
- Long-press Confirm = direct bookmark
- Save Passage = menu-only, starts capture
- Bookmark list browser with delete confirmation
- Duplicate skip by bookPercent

### Step 1: Write the design doc

(Content should be a concise summary of the design decisions made during brainstorming.)

### Step 2: Commit

```bash
git add docs/plans/2026-02-06-bookmark-navigation-design.md
git commit -m "docs: add bookmark navigation design doc"
```

---

## Task 8: Final verification and cleanup

### Step 1: Run formatter on all changed files

```bash
cd /Users/tobi/dotfiles/crosspoint-reader && /opt/homebrew/opt/llvm/bin/clang-format -i \
  src/BookmarkStore.h \
  src/BookmarkStore.cpp \
  src/activities/reader/EpubReaderActivity.h \
  src/activities/reader/EpubReaderActivity.cpp \
  src/activities/reader/EpubReaderMenuActivity.h \
  src/activities/reader/EpubReaderBookmarkListActivity.h \
  src/activities/reader/EpubReaderBookmarkListActivity.cpp
```

### Step 2: Full build

Run: `cd /Users/tobi/dotfiles/crosspoint-reader && pio run 2>&1 | tail -10`
Expected: Compiles without errors or warnings.

### Step 3: Flash to device

Run: `cd /Users/tobi/dotfiles/crosspoint-reader && pio run --target upload --upload-port /dev/cu.usbmodem2101 2>&1 | tail -5`
Expected: Successful upload.

### Step 4: Manual test checklist

1. **Bookmark:** Hold Confirm → "Bookmarked" appears in status bar. Verify `/Bookmarks/<book>.bookmarks` file created on SD card.
2. **Duplicate skip:** Hold Confirm again on same page → "Bookmarked" appears (no duplicate entry in file).
3. **Bookmark list:** Press Confirm → select "Bookmarks" → see list with percentage entries.
4. **Jump to bookmark:** Select a bookmark → press Confirm → reader jumps to correct position.
5. **Delete bookmark:** Long-press Back → title changes to "Delete bookmark?" → press Confirm → bookmark removed.
6. **Cancel delete:** Long-press Back → press Back → returns to normal list.
7. **Empty list:** Delete all bookmarks → shows "No bookmarks" message.
8. **Save Passage:** Press Confirm → select "Save Passage" → "Capture started" → turn pages → Hold Confirm → "Passage saved". Verify `/Saved Passages/<book>.md` file.
9. **Stop Capture from menu:** Start capture → Press Confirm → "Stop Capture" appears → select it → passage saved.
10. **Long-press Confirm during capture:** Still stops capture (unchanged behavior).
11. **Back button:** Short press Back exits reader. Long press Back goes home.

### Step 5: Amend last commit if formatter made changes

If the formatter changed anything, stage and commit:
```bash
git add -A && git commit -m "style: apply clang-format"
```

---

## Verification Summary

| Test | Expected |
|------|----------|
| Build compiles | No errors |
| Long-press Confirm | Shows "Bookmarked" (not popup) |
| Menu shows "Save Passage" | Not "Bookmark & Save" |
| Menu shows "Bookmarks" | Opens bookmark list |
| Bookmark list navigation | Vol Up/Down moves cursor |
| Bookmark list jump | Confirm jumps to position |
| Bookmark list delete | Long-press Back → confirmation → delete |
| Save Passage from menu | Starts capture directly |
| Stop Capture from menu | Stops and saves passage |
| Binary file created | `/Bookmarks/<book>.bookmarks` exists |
| Duplicate handling | Same position not added twice |
| Passage file unchanged | `/Saved Passages/<book>.md` still works |
