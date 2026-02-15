# Page Text Export — Design

Save the current page's rendered text to a `.txt` file on the SD card, providing a lightweight way to capture passages for later reference without requiring touch-based text selection.

Related discussion: https://github.com/crosspoint-reader/crosspoint-reader/discussions/481

## User Interaction

**Trigger:** Hold the Confirm button for ~1 second while in the reading view. Short-press behavior (open reader menu) is preserved — it fires on release only if hold duration is under the threshold.

**Flow:**
1. User reads a page they want to save
2. User holds Confirm for 1s
3. Status bar briefly shows "Page saved" (or "Save failed" on error)
4. Status bar returns to normal on the next page turn
5. User continues reading — no interruption

Multi-page capture is not supported in v1. To save consecutive pages, the user captures each one individually.

## Storage & File Format

**Location:** `.crosspoint/exports/` on the SD card.

**Filename:** Derived from the book title, sanitized for FAT32 compatibility. Fallback to epub hash if title is unavailable.

```
.crosspoint/exports/The_Great_Gatsby.txt
```

**Format:** Plain UTF-8 text. Header written once on file creation, each capture appended with a metadata separator:

```
== The Great Gatsby — F. Scott Fitzgerald ==

--- Chapter 3: The End of Something | Page 42 | 18% ---
In my younger and more vulnerable years my father
gave me some advice that I've been turning over in
my mind ever since...

--- Chapter 5: The Party | Page 87 | 38% ---
The lights grow brighter as the earth lurches away
from the sun, and now the orchestra is playing...
```

## Implementation

### Files to change

1. **`EpubReaderActivity`** — Add 1000ms long-press detection on Confirm in the `update()` loop, similar to the existing `longPressChapterSkip` pattern (700ms on page buttons).

2. **New `PageExporter` utility** — Small class responsible for:
   - Sanitizing book title into a valid FAT32 filename
   - Creating `.crosspoint/exports/` directory if needed
   - Writing the header on first capture (if file doesn't exist)
   - Appending separator + metadata + page text
   - Returning success/failure for status bar feedback

3. **Status bar feedback** — Set a temporary flag after capture that the status bar renderer checks. Renders "Page saved" or "Save failed" instead of normal content. Clears on next page turn or render cycle.

### Data available at capture time

All already accessible in `EpubReaderActivity`:
- Current page text (from rendered `Section` data)
- Chapter name (from table of contents / spine)
- Page number and total (tracked for progress)
- Book title and author (from `BookMetadataCache`)
- Percentage (calculated for status bar)

### What's not needed

- No new dependencies or external libraries
- No changes to the caching system or section format
- No new activities or menu screens
- No framebuffer manipulation

### Estimated scope

~3 files touched, ~150-200 lines of new code.
