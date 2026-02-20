# Page Text Export — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Let users save the current page's text to a `.txt` file on the SD card by long-pressing the Confirm button during reading.

**Architecture:** Add a `getPlainText()` method to `TextBlock` and `Page`, a new `PageExporter` utility that appends text to per-book export files, a long-press handler on Confirm in `EpubReaderActivity`, and temporary status bar override feedback. No new screens, activities, or dependencies.

**Tech Stack:** C++ (PlatformIO/ESP32-C3), SdFat library for SD card access, FreeRTOS for thread safety.

---

### Task 1: Add `getPlainText()` to TextBlock

`TextBlock::words` is private. We need a public method to reconstruct the line's text.

**Files:**
- Modify: `lib/Epub/Epub/blocks/TextBlock.h:20-36`
- Modify: `lib/Epub/Epub/blocks/TextBlock.cpp`

**Step 1: Add method declaration to TextBlock.h**

In `lib/Epub/Epub/blocks/TextBlock.h`, add after line 30 (`bool isEmpty()`):

```cpp
  std::string getPlainText() const;
```

**Step 2: Implement in TextBlock.cpp**

In `lib/Epub/Epub/blocks/TextBlock.cpp`, add at end of file:

```cpp
std::string TextBlock::getPlainText() const {
  std::string result;
  for (auto it = words.begin(); it != words.end(); ++it) {
    if (it != words.begin()) {
      result += ' ';
    }
    result += *it;
  }
  return result;
}
```

**Step 3: Commit**

```bash
git add lib/Epub/Epub/blocks/TextBlock.h lib/Epub/Epub/blocks/TextBlock.cpp
git commit -m "feat: add getPlainText() to TextBlock for text extraction"
```

---

### Task 2: Add `getPlainText()` to Page

`PageLine::block` is also private. Add a text extraction method that walks all elements.

**Files:**
- Modify: `lib/Epub/Epub/Page.h:24-43`
- Modify: `lib/Epub/Epub/Page.cpp`

**Step 1: Add getter to PageLine and method to Page**

In `lib/Epub/Epub/Page.h`, add to `PageLine` class after line 31:

```cpp
  const TextBlock& getBlock() const { return *block; }
```

Add to `Page` class after line 40:

```cpp
  std::string getPlainText() const;
```

**Step 2: Implement Page::getPlainText() in Page.cpp**

In `lib/Epub/Epub/Page.cpp`, add at end of file:

```cpp
std::string Page::getPlainText() const {
  std::string result;
  for (const auto& element : elements) {
    auto* pageLine = dynamic_cast<PageLine*>(element.get());
    if (pageLine) {
      if (!result.empty()) {
        result += '\n';
      }
      result += pageLine->getBlock().getPlainText();
    }
  }
  return result;
}
```

Note: include `<string>` at top of Page.cpp if not already present.

**Step 3: Build to verify compilation**

```bash
pio run
```

Expected: Compiles without errors. (No runtime test possible without device, but compilation confirms API correctness.)

**Step 4: Commit**

```bash
git add lib/Epub/Epub/Page.h lib/Epub/Epub/Page.cpp
git commit -m "feat: add getPlainText() to Page for full page text extraction"
```

---

### Task 3: Create PageExporter utility

Handles filename sanitization, directory creation, file header, and append-mode text writing.

**Files:**
- Create: `src/PageExporter.h`
- Create: `src/PageExporter.cpp`

**Step 1: Write PageExporter.h**

```cpp
#pragma once
#include <string>

// Exports the current page's text to a per-book .txt file on the SD card.
// Files are stored at /.crosspoint/exports/<sanitized-title>.txt
// Each capture is appended with chapter/page/percentage metadata.
class PageExporter {
 public:
  // Export a page's text. Returns true on success.
  // - bookTitle: e.g. "The Great Gatsby"
  // - bookAuthor: e.g. "F. Scott Fitzgerald"
  // - bookHash: fallback filename if title is empty
  // - chapterTitle: e.g. "Chapter 3: The River"
  // - pageNumber: 1-based page within chapter
  // - bookPercent: 0-100 overall book progress
  // - pageText: the plain text content of the page
  static bool exportPage(const std::string& bookTitle, const std::string& bookAuthor,
                         const std::string& bookHash, const std::string& chapterTitle,
                         int pageNumber, int bookPercent, const std::string& pageText);

 private:
  static std::string sanitizeFilename(const std::string& title);
  static std::string getExportPath(const std::string& bookTitle, const std::string& bookHash);
  static bool writeHeader(FsFile& file, const std::string& bookTitle, const std::string& bookAuthor);
  static bool writeEntry(FsFile& file, const std::string& chapterTitle, int pageNumber,
                         int bookPercent, const std::string& pageText);
};
```

**Step 2: Write PageExporter.cpp**

```cpp
#include "PageExporter.h"

#include <SDCardManager.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr const char* EXPORTS_DIR = "/.crosspoint/exports";
constexpr const char* TAG = "PEX";
}  // namespace

std::string PageExporter::sanitizeFilename(const std::string& title) {
  std::string result;
  result.reserve(title.size());
  for (char c : title) {
    if (c == ' ') {
      result += '_';
    } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
      result += c;
    }
    // Skip all other characters (punctuation, special chars, etc.)
  }
  // Truncate to 80 chars to stay well within FAT32 limits
  if (result.size() > 80) {
    result.resize(80);
  }
  if (result.empty()) {
    result = "untitled";
  }
  return result;
}

std::string PageExporter::getExportPath(const std::string& bookTitle, const std::string& bookHash) {
  std::string filename;
  if (bookTitle.empty()) {
    filename = bookHash;
  } else {
    filename = sanitizeFilename(bookTitle);
  }
  return std::string(EXPORTS_DIR) + "/" + filename + ".txt";
}

bool PageExporter::writeHeader(FsFile& file, const std::string& bookTitle, const std::string& bookAuthor) {
  std::string header = "== " + bookTitle;
  if (!bookAuthor.empty()) {
    header += " \xe2\x80\x94 " + bookAuthor;  // em-dash UTF-8
  }
  header += " ==\n";
  return file.write(reinterpret_cast<const uint8_t*>(header.c_str()), header.size()) == header.size();
}

bool PageExporter::writeEntry(FsFile& file, const std::string& chapterTitle, int pageNumber, int bookPercent,
                              const std::string& pageText) {
  char meta[128];
  snprintf(meta, sizeof(meta), "\n--- %s | Page %d | %d%% ---\n", chapterTitle.c_str(), pageNumber, bookPercent);
  std::string entry(meta);
  entry += pageText;
  entry += '\n';
  return file.write(reinterpret_cast<const uint8_t*>(entry.c_str()), entry.size()) == entry.size();
}

bool PageExporter::exportPage(const std::string& bookTitle, const std::string& bookAuthor,
                              const std::string& bookHash, const std::string& chapterTitle,
                              int pageNumber, int bookPercent, const std::string& pageText) {
  SdMan.mkdir(EXPORTS_DIR);

  const std::string path = getExportPath(bookTitle, bookHash);

  // Check if file already exists (to decide whether to write header)
  const bool isNew = !SdMan.exists(path.c_str());

  // Open in append mode
  FsFile file = SdMan.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    Serial.printf("[%lu] [%s] Failed to open export file: %s\n", millis(), TAG, path.c_str());
    return false;
  }

  bool ok = true;
  if (isNew) {
    ok = writeHeader(file, bookTitle, bookAuthor);
  }
  if (ok) {
    ok = writeEntry(file, chapterTitle, pageNumber, bookPercent, pageText);
  }

  file.close();

  if (ok) {
    Serial.printf("[%lu] [%s] Page exported to %s\n", millis(), TAG, path.c_str());
  } else {
    Serial.printf("[%lu] [%s] Failed to write export entry\n", millis(), TAG);
  }
  return ok;
}
```

**Step 3: Build to verify compilation**

```bash
pio run
```

Expected: Compiles without errors.

**Step 4: Commit**

```bash
git add src/PageExporter.h src/PageExporter.cpp
git commit -m "feat: add PageExporter utility for saving page text to SD card"
```

---

### Task 4: Add status bar override to EpubReaderActivity

Add a `statusBarOverride` string that, when non-empty, replaces the normal status bar content. Clears on next page turn.

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.h:11-56`
- Modify: `src/activities/reader/EpubReaderActivity.cpp:711-818` (renderStatusBar)
- Modify: `src/activities/reader/EpubReaderActivity.cpp:264-289` (page turn — clear override)

**Step 1: Add member variable to EpubReaderActivity.h**

After line 29 (`bool skipNextButtonCheck = false;`), add:

```cpp
  std::string statusBarOverride;  // Temporary override text (e.g. "Page saved"), cleared on page turn
```

**Step 2: Add override check at top of renderStatusBar()**

In `EpubReaderActivity.cpp`, at the beginning of `renderStatusBar()` (after line 713, `auto metrics = ...`), add:

```cpp
  if (!statusBarOverride.empty()) {
    const auto screenHeight = renderer.getScreenHeight();
    const auto textY = screenHeight - orientedMarginBottom - 4;
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, statusBarOverride.c_str());
    const int x = (renderer.getScreenWidth() - textWidth) / 2;
    renderer.drawText(SMALL_FONT_ID, x, textY, statusBarOverride.c_str());
    return;
  }
```

**Step 3: Clear override on page turn**

In `EpubReaderActivity.cpp`, just before the existing `if (!prevTriggered && !nextTriggered)` check at line 233, add:

```cpp
  // Clear any status bar override on page turn
  if (prevTriggered || nextTriggered) {
    statusBarOverride.clear();
  }
```

**Step 4: Build to verify compilation**

```bash
pio run
```

Expected: Compiles. Behavior unchanged since `statusBarOverride` is always empty at this point.

**Step 5: Commit**

```bash
git add src/activities/reader/EpubReaderActivity.h src/activities/reader/EpubReaderActivity.cpp
git commit -m "feat: add temporary status bar override mechanism"
```

---

### Task 5: Wire up Confirm long-press to export page text

Replace the current simple `wasReleased(Confirm)` check with a long-press/short-press split. Long-press (1000ms) triggers page export. Short-press (<1000ms) still opens the reader menu.

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.cpp:19-24` (add constant)
- Modify: `src/activities/reader/EpubReaderActivity.cpp:1-17` (add include)
- Modify: `src/activities/reader/EpubReaderActivity.cpp:187-205` (Confirm button logic)

**Step 1: Add include and constant**

In `EpubReaderActivity.cpp`, add include after line 4 (`#include <FsHelpers.h>`):

```cpp
#include "PageExporter.h"
```

In the anonymous namespace (after line 22, `constexpr unsigned long goHomeMs = 1000;`), add:

```cpp
constexpr unsigned long exportPageMs = 1000;
```

**Step 2: Replace Confirm button handling**

Replace lines 187-205 (the current `if (mappedInput.wasReleased(MappedInputManager::Button::Confirm))` block) with:

```cpp
  // Long press CONFIRM (1s+) exports current page text
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= exportPageMs) {
    if (section && epub) {
      auto page = section->loadPageFromSectionFile();
      if (page) {
        const std::string pageText = page->getPlainText();
        const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
        const std::string chapterTitle = (tocIndex >= 0) ? epub->getTocItem(tocIndex).title : "Unnamed";
        const int pageNum = section->currentPage + 1;
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        const int bookPercent = clampPercent(
            static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f));
        const std::string bookHash = epub->getCachePath().substr(epub->getCachePath().rfind('/') + 1);

        const bool ok = PageExporter::exportPage(epub->getTitle(), epub->getAuthor(), bookHash, chapterTitle, pageNum,
                                                 bookPercent, pageText);
        statusBarOverride = ok ? "Page saved" : "Save failed";
      } else {
        statusBarOverride = "Save failed";
      }
      updateRequired = true;
    }
    return;
  }

  // Short press CONFIRM opens reader menu
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() < exportPageMs) {
    // Don't start activity transition while rendering
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    const int currentPage = section ? section->currentPage + 1 : 0;
    const int totalPages = section ? section->pageCount : 0;
    float bookProgress = 0.0f;
    if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
    }
    const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
    exitActivity();
    enterNewActivity(new EpubReaderMenuActivity(
        this->renderer, this->mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
        SETTINGS.orientation, [this](const uint8_t orientation) { onReaderMenuBack(orientation); },
        [this](EpubReaderMenuActivity::MenuAction action) { onReaderMenuConfirm(action); }));
    xSemaphoreGive(renderingMutex);
  }
```

**Step 3: Build**

```bash
pio run
```

Expected: Compiles without errors.

**Step 4: Flash and test on device**

```bash
pio run --target upload
```

Test procedure:
1. Open any book, navigate to a page with text
2. Short-press Confirm → reader menu should open (existing behavior preserved)
3. Go back to reading, hold Confirm for ~1 second → status bar should show "Page saved"
4. Turn to next page → status bar returns to normal
5. Connect device to computer, check SD card for `/.crosspoint/exports/<book-title>.txt`
6. Open the file — verify it has the header and one entry with chapter/page/percentage metadata and the page text
7. Repeat capture on another page — verify it appends a second entry to the same file

**Step 5: Commit**

```bash
git add src/activities/reader/EpubReaderActivity.cpp
git commit -m "feat: wire up Confirm long-press to export page text"
```

---

### Task 6: Final build and cleanup

**Step 1: Full clean build**

```bash
pio run --target clean && pio run
```

Expected: Clean compile, no warnings related to our changes.

**Step 2: Verify no stale includes or unused code**

Review that:
- `PageExporter.h` is only included in `EpubReaderActivity.cpp`
- `getPlainText()` is called from `EpubReaderActivity.cpp`
- No unused variables or dead code paths

**Step 3: Final commit (if any cleanup needed)**

```bash
git add -A
git commit -m "chore: cleanup page text export implementation"
```
