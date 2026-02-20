# Bookmark & Save Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the direct long-press-to-capture behavior with a popup menu offering "Bookmark" and "Save Passage" options.

**Architecture:** Add a `POPUP_MENU` state to the existing capture state machine in `EpubReaderActivity`. When in this state, render a 2-item overlay inline (no new Activity class) and handle up/down selection. Bookmarks reuse `PageExporter::exportPassage()` with marker text. The reader menu item is renamed from "Save Passage" to "Bookmark & Save" and triggers the same popup.

**Tech Stack:** C++ (ESP32/Arduino), GfxRenderer for drawing, existing PageExporter for file I/O.

---

## Task 1: Add POPUP_MENU state and popup members to header

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.h:38-41`

**Step 1: Update CaptureState enum and add popup members**

Change line 38 from:
```cpp
  enum class CaptureState { IDLE, CAPTURING };
```
to:
```cpp
  enum class CaptureState { IDLE, POPUP_MENU, CAPTURING };
```

Add after line 41 (`bool statusBarMarker`):
```cpp
  int popupSelectedIndex = 0;  // 0 = Bookmark, 1 = Save Passage
```

Add a new private method declaration after `cancelCapture()` (line 64):
```cpp
  void writeBookmark();
  void renderPopupMenu() const;
```

**Step 2: Commit**

```bash
git add src/activities/reader/EpubReaderActivity.h
git commit -m "feat: add POPUP_MENU state and popup members to EpubReaderActivity"
```

---

## Task 2: Implement renderPopupMenu()

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.cpp` (add new method)

**Step 1: Add renderPopupMenu() method**

Add after the `cancelCapture()` method (after line 198):

```cpp
void EpubReaderActivity::renderPopupMenu() const {
  constexpr int margin = 12;
  constexpr int y = 60;
  constexpr int itemCount = 2;
  const char* labels[itemCount] = {"Bookmark", "Save Passage"};

  // Calculate popup dimensions based on widest label
  int maxTextWidth = 0;
  for (int i = 0; i < itemCount; i++) {
    const int w = renderer.getTextWidth(UI_12_FONT_ID, labels[i], EpdFontFamily::BOLD);
    if (w > maxTextWidth) {
      maxTextWidth = w;
    }
  }
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int rowHeight = lineHeight + 8;
  const int popupWidth = maxTextWidth + margin * 2;
  const int popupHeight = rowHeight * itemCount + margin * 2;
  const int popupX = (renderer.getScreenWidth() - popupWidth) / 2;

  // Draw popup border and background
  renderer.fillRect(popupX - 2, y - 2, popupWidth + 4, popupHeight + 4, true);
  renderer.fillRect(popupX, y, popupWidth, popupHeight, false);

  // Draw each item
  for (int i = 0; i < itemCount; i++) {
    const int itemY = y + margin + i * rowHeight;
    const bool selected = (i == popupSelectedIndex);
    if (selected) {
      renderer.fillRect(popupX + 2, itemY - 2, popupWidth - 4, rowHeight, true);
    }
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, labels[i], EpdFontFamily::BOLD);
    const int textX = popupX + (popupWidth - textWidth) / 2;
    renderer.drawText(UI_12_FONT_ID, textX, itemY, labels[i], !selected, EpdFontFamily::BOLD);
  }
}
```

**Step 2: Build to verify it compiles**

Run: `pio run`
Expected: Compiles without errors (method is defined but not yet called).

**Step 3: Commit**

```bash
git add src/activities/reader/EpubReaderActivity.cpp
git commit -m "feat: implement renderPopupMenu() for bookmark/save popup"
```

---

## Task 3: Implement writeBookmark()

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.cpp` (add new method)

**Step 1: Add writeBookmark() method**

Add after the `renderPopupMenu()` method:

```cpp
void EpubReaderActivity::writeBookmark() {
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
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  const std::string chapterTitle = (tocIndex >= 0) ? epub->getTocItem(tocIndex).title : "Unnamed";
  const float chapterProgress = (section->pageCount > 0)
                                    ? static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount)
                                    : 0.0f;
  const int bookPercent =
      clampPercent(static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f));
  const int chapterPercent = clampPercent(static_cast<int>(chapterProgress * 100.0f + 0.5f));
  xSemaphoreGive(renderingMutex);

  std::vector<CapturedPage> bookmark = {{"\xf0\x9f\x93\x8c Bookmarked", chapterTitle, bookPercent, chapterPercent}};
  const bool ok = PageExporter::exportPassage(epub->getPath(), epub->getTitle(), epub->getAuthor(), bookmark);
  statusBarOverride = ok ? "Bookmarked" : "Bookmark failed";
  updateRequired = true;
}
```

Note: `"\xf0\x9f\x93\x8c"` is the UTF-8 encoding of the bookmark emoji. If the device font doesn't render it, it will be harmlessly skipped in the .md file — the text "Bookmarked" still appears. The emoji is only for the .md file readability on desktop, not displayed on the e-ink screen.

**Step 2: Build to verify it compiles**

Run: `pio run`
Expected: Compiles without errors.

**Step 3: Commit**

```bash
git add src/activities/reader/EpubReaderActivity.cpp
git commit -m "feat: implement writeBookmark() for single-page bookmarks"
```

---

## Task 4: Wire up popup state machine in loop()

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.cpp:253-265` (long-press handler)
- Modify: `src/activities/reader/EpubReaderActivity.cpp:232-237` (pending capture handler)

**Step 1: Add popup input handling block**

Add a new block in `loop()` after the `skipNextButtonCheck` block (after line 251) and BEFORE the existing long-press Confirm handler:

```cpp
  // Popup menu input handling — intercepts all input while popup is visible
  if (captureState == CaptureState::POPUP_MENU) {
    // Navigate up
    if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
        mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
      popupSelectedIndex = 0;
      updateRequired = true;
    }
    // Navigate down
    if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
        mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
      popupSelectedIndex = 1;
      updateRequired = true;
    }
    // Confirm selection
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      captureState = CaptureState::IDLE;
      if (popupSelectedIndex == 0) {
        writeBookmark();
      } else {
        startCapture();
      }
      return;
    }
    // Dismiss popup
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      captureState = CaptureState::IDLE;
      updateRequired = true;
    }
    return;
  }
```

**Step 2: Change long-press Confirm in IDLE to show popup instead of starting capture**

Change lines 253-265 from:
```cpp
  // Long press CONFIRM (1s+) toggles capture
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= captureHoldMs) {
    if (captureState == CaptureState::IDLE) {
      if (section && epub) {
        startCapture();
      }
    } else {
      stopCapture();
    }
    // Wait for button release before processing further input
    skipNextButtonCheck = true;
    return;
  }
```

to:
```cpp
  // Long press CONFIRM (1s+): show popup (IDLE) or stop capture (CAPTURING)
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= captureHoldMs) {
    if (captureState == CaptureState::IDLE) {
      if (section && epub) {
        captureState = CaptureState::POPUP_MENU;
        popupSelectedIndex = 0;
        updateRequired = true;
      }
    } else if (captureState == CaptureState::CAPTURING) {
      stopCapture();
    }
    // Wait for button release before processing further input
    skipNextButtonCheck = true;
    return;
  }
```

**Step 3: Update the pending capture handler from menu**

Change the `pendingStartCapture` block (lines 232-237) from:
```cpp
  // Handle deferred capture start from menu
  if (pendingStartCapture) {
    pendingStartCapture = false;
    startCapture();
    return;
  }
```

to:
```cpp
  // Handle deferred popup from menu
  if (pendingStartCapture) {
    pendingStartCapture = false;
    if (section && epub) {
      captureState = CaptureState::POPUP_MENU;
      popupSelectedIndex = 0;
      updateRequired = true;
    }
    return;
  }
```

**Step 4: Build to verify**

Run: `pio run`
Expected: Compiles without errors.

**Step 5: Commit**

```bash
git add src/activities/reader/EpubReaderActivity.cpp
git commit -m "feat: wire popup state machine into reader loop"
```

---

## Task 5: Render popup in displayTaskLoop

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.cpp:644-663` (inside `renderScreen()`)

**Step 1: Add popup rendering after the normal page render**

The popup needs to render on top of the page content. In `renderScreen()`, after the page is rendered and `displayBuffer()` is called (after line 833, the `restoreBwBuffer()` call), add:

```cpp
  // Draw popup overlay if active
  if (captureState == CaptureState::POPUP_MENU) {
    renderPopupMenu();
    renderer.displayBuffer();
  }
```

**Step 2: Build and flash to test**

Run: `pio run --target upload`

**Step 3: Manual test**

1. Open a book
2. Long-press Confirm — popup should appear with "Bookmark" highlighted
3. Press Right/Down — "Save Passage" should highlight
4. Press Back — popup should dismiss
5. Long-press Confirm → press Confirm on "Bookmark" — "Bookmarked" should flash
6. Long-press Confirm → select "Save Passage" → "Capture started" should appear, * marker visible
7. Long-press Confirm again → "Passage saved" should appear

**Step 4: Commit**

```bash
git add src/activities/reader/EpubReaderActivity.cpp
git commit -m "feat: render popup overlay in display task"
```

---

## Task 6: Rename menu item and update menu handler

**Files:**
- Modify: `src/activities/reader/EpubReaderMenuActivity.h:53`
- Modify: `src/activities/reader/EpubReaderMenuActivity.h:32-37` (isCapturing label)
- Modify: `src/activities/reader/EpubReaderActivity.cpp:513-522` (START_CAPTURE handler)

**Step 1: Rename "Save Passage" to "Bookmark & Save" in menuItems**

Change line 53 from:
```cpp
                                     {MenuAction::START_CAPTURE, "Save Passage"},
```
to:
```cpp
                                     {MenuAction::START_CAPTURE, "Bookmark & Save"},
```

**Step 2: Update the isCapturing label override**

The constructor already changes the label to "Stop Capture" when capturing. This is still correct — when `CAPTURING`, the menu item should say "Stop Capture" (which directly stops the capture, no popup). No change needed here.

**Step 3: Update the START_CAPTURE menu handler**

In `EpubReaderActivity.cpp`, change the `START_CAPTURE` case (lines 513-522) from:
```cpp
    case EpubReaderMenuActivity::MenuAction::START_CAPTURE: {
      exitActivity();
      applyOrientation(SETTINGS.orientation);
      if (captureState == CaptureState::CAPTURING) {
        stopCapture();
      } else {
        pendingStartCapture = true;
      }
      break;
    }
```

to:
```cpp
    case EpubReaderMenuActivity::MenuAction::START_CAPTURE: {
      exitActivity();
      applyOrientation(SETTINGS.orientation);
      if (captureState == CaptureState::CAPTURING) {
        stopCapture();
      } else {
        pendingStartCapture = true;  // Opens the popup menu after returning to reader
      }
      break;
    }
```

The logic is unchanged — `pendingStartCapture` now opens the popup instead of directly starting capture (handled by the change in Task 4 Step 3). Just add the clarifying comment.

**Step 4: Build to verify**

Run: `pio run`
Expected: Compiles without errors.

**Step 5: Commit**

```bash
git add src/activities/reader/EpubReaderMenuActivity.h src/activities/reader/EpubReaderActivity.cpp
git commit -m "feat: rename menu item to 'Bookmark & Save', trigger popup from menu"
```

---

## Task 7: Update USER_GUIDE.md

**Files:**
- Modify: `USER_GUIDE.md:19-22` (table of contents)
- Modify: `USER_GUIDE.md:202-224` (Saving Passages section)
- Modify: `USER_GUIDE.md:229` (System Navigation mention)

**Step 1: Update table of contents**

Change line 22 from:
```markdown
    - [Saving Passages](#saving-passages)
```
to:
```markdown
    - [Bookmarks & Saved Passages](#bookmarks--saved-passages)
```

**Step 2: Replace the "Saving Passages" section (lines 202-224)**

Replace with:
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

**Step 3: Update System Navigation mention**

Change line 229 from:
```markdown
* **Reader Menu:** Press **Confirm** to open the reader menu (chapter selection, save passage, and more).
```
to:
```markdown
* **Reader Menu:** Press **Confirm** to open the reader menu (chapter selection, bookmark & save, and more).
```

**Step 4: Commit**

```bash
git add USER_GUIDE.md
git commit -m "docs: update user guide for bookmark & save feature"
```

---

## Task 8: Format, build, flash, and final test

**Step 1: Run formatter**

Run: `bin/clang-format-fix`

**Step 2: Build**

Run: `pio run`
Expected: Compiles without errors or warnings.

**Step 3: Flash**

Run: `pio run --target upload`

**Step 4: Manual verification checklist**

1. **Popup from long-press:** Hold Confirm → popup appears with "Bookmark" / "Save Passage"
2. **Navigation:** Left selects "Bookmark", Right selects "Save Passage"
3. **Dismiss:** Back dismisses popup, returns to reading
4. **Bookmark:** Select "Bookmark" → "Bookmarked" flash → check `.md` file has bookmark entry
5. **Save Passage:** Select "Save Passage" → "Capture started" → turn pages → Hold Confirm → "Passage saved"
6. **Menu entry:** Press Confirm → select "Bookmark & Save" → popup appears
7. **Stop Capture from menu:** Start capture → open menu → "Stop Capture" shown → select it → passage saved
8. **Short-press Confirm still opens menu** when not capturing
9. **Back button exits reader** normally
10. **File format:** Open `.md` file — bookmarks show as `📌 Bookmarked`, passages show page text

**Step 5: Commit any formatter changes**

```bash
git add -A
git commit -m "style: apply clang-format"
```

**Step 6: Push**

```bash
git push fork feature/page-text-export
```
