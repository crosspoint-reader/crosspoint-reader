# Bookmark & Save — Design

Add a long-press Confirm popup with two actions: bookmark the current page or start a multi-page passage capture. Replaces the current direct-to-capture long-press behavior.

Follows from PR #714 review feedback: long-press Confirm should support bookmarks (like stock firmware), not just passage capture.

## User Interaction

**Trigger:** Hold Confirm for ~1 second while reading.

**Flow:**
1. User holds Confirm → a small popup appears with two options:
   - **Bookmark** (selected by default)
   - **Save Passage**
2. User navigates with Left (up) / Right (down)
3. User presses Confirm to execute the selected action, or Back to dismiss

**Bookmark action:**
- Writes a bookmark entry to the same per-book `.md` file used by passage capture
- Status bar flashes "Bookmarked"
- Returns to reading immediately

**Save Passage action:**
- Enters `CAPTURING` state (same as current behavior)
- Status bar flashes "Capture started", persistent `*` marker appears
- Multi-page capture flow continues as before

**Reader menu:** The existing "Save Passage" menu item is renamed to "Bookmark & Save" and triggers the same popup (instead of directly starting capture).

**While capturing:** Long-press Confirm still stops the capture directly (no popup). The popup only appears in `IDLE` state.

## Popup UI

A minimal overlay rendered inline by `EpubReaderActivity`, not a subactivity.

```
+---------------------+
|     Bookmark        |  ← highlighted (inverted)
|     Save Passage    |
+---------------------+
```

- Centered horizontally, positioned near top of screen (y=60, matching existing `drawPopup` style)
- Selected item rendered with inverted colors (white text on black)
- Unselected item rendered normally (black text on white)
- Black border, 2px thickness (matching existing popup style)

## Bookmark File Format

Bookmarks are written to the same `/Saved Passages/<book-filename>.md` file as passages. A bookmark is a passage entry with marker text instead of page content:

```markdown
## III: The Shadow | 72% of book | 2% of chapter
Bookmarked

---
```

This keeps one file per book for all annotations (bookmarks + passages), browsable on device and desktop.

## State Machine Changes

```
IDLE + long-press Confirm → POPUP_MENU
POPUP_MENU + Confirm (Bookmark) → write bookmark, flash "Bookmarked" → IDLE
POPUP_MENU + Confirm (Save Passage) → startCapture() → CAPTURING
POPUP_MENU + Back → IDLE (dismiss, no action)
CAPTURING + long-press Confirm → stopCapture() → IDLE (unchanged)
CAPTURING + page forward → append page to buffer (unchanged)
CAPTURING + page backward → stopCapture(), then page back (unchanged)
CAPTURING + Back (exit reader) → cancelCapture() → IDLE (unchanged)
```

## Implementation

### Changes to EpubReaderActivity.h

- Add `POPUP_MENU` to `CaptureState` enum
- Add `int popupSelectedIndex = 0` (0 = Bookmark, 1 = Save Passage)

### Changes to EpubReaderActivity.cpp

**Long-press Confirm (in `loop()`):**
- `IDLE`: set `captureState = POPUP_MENU`, `popupSelectedIndex = 0`, `updateRequired = true`
- `POPUP_MENU`: no-op (already showing popup)
- `CAPTURING`: `stopCapture()` (unchanged)

**New input block (in `loop()`, before existing input handling):**
```
if captureState == POPUP_MENU:
  Left  → popupSelectedIndex = 0, updateRequired = true
  Right → popupSelectedIndex = 1, updateRequired = true
  Confirm →
    if index == 0: write bookmark via PageExporter, flash "Bookmarked"
    if index == 1: startCapture()
    captureState = IDLE (for bookmark) or CAPTURING (for save)
  Back → captureState = IDLE, updateRequired = true
  return (skip all other input handling)
```

**New render function (`renderPopupMenu()`):**
- Draw bordered box with two text items
- Highlight selected item with inverted colors
- Called from the display task when `captureState == POPUP_MENU`

### Changes to EpubReaderMenuActivity.h

- Rename `"Save Passage"` label to `"Bookmark & Save"`

### Changes to EpubReaderActivity.cpp (menu handler)

- `onReaderMenuConfirm(START_CAPTURE)`: instead of calling `startCapture()` directly, set `captureState = POPUP_MENU` and `popupSelectedIndex = 0`

### Changes to PageExporter

No changes needed. A bookmark is just `exportPassage()` called with a single `CapturedPage` where `pageText = "Bookmarked"`.

### Changes to USER_GUIDE.md

- Update "Saving Passages" section to cover both bookmarking and saving
- Document the long-press popup and its two options
- Rename references from "Save Passage" to "Bookmark & Save"

## Out of Scope

- **Bookmark page indicator:** Showing a marker on bookmarked pages requires a stable page identity system. Page indices shift with font/margin changes, making matching unreliable. Deferred to a future PR.
- **Bookmark list/navigation:** Browsing and jumping to bookmarks from within the device. Future feature.
- **Bookmark deletion:** Removing individual bookmarks from the `.md` file. Future feature.

## Estimated Scope

~4 files changed, ~100-120 lines of new code (mostly the popup render function and input block).
