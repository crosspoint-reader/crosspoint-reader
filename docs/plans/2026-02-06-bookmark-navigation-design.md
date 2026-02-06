# Bookmark Navigation — Design

Separate bookmarks from saved passages. Bookmarks are lightweight position markers stored in binary format; passages are text captures stored as Markdown.

## Storage

**Bookmarks:** Binary files at `/Bookmarks/<book-filename>.bookmarks`
- Format: `[version:1][count:1][entries: count * 5 bytes]`
- Each entry: `[bookPercent:1][spineIndex:2 LE][pageIndex:2 LE]`
- Max 255 entries per book (uint8_t count)
- Duplicates skipped by bookPercent match

**Passages:** Unchanged — Markdown files at `/Saved Passages/<book-filename>.md`

## User Interaction

**Creating a bookmark:** Hold Confirm (~1s) while reading. Status bar flashes "Bookmarked". If a bookmark at the same book percentage already exists, it's silently skipped.

**Viewing bookmarks:** Reader menu → Bookmarks. Opens a list showing `N% of book` for each entry.

**In the bookmark list:**
- Up/Down to navigate (with long-press page skip)
- Confirm to jump to the bookmarked position
- Long-press Back to delete (with confirmation screen)
- Back to return to reading

**Saving passages:** Reader menu → Save Passage. Starts multi-page capture (unchanged from previous implementation).

## Changes from Previous Design

The earlier "Bookmark & Save" popup (long-press Confirm → choose Bookmark or Save Passage) is removed. Instead:
- Long-press Confirm = direct bookmark (no popup)
- Save Passage = reader menu only
- New "Bookmarks" menu item for browsing

## Implementation

| Component | Purpose |
|-----------|---------|
| `BookmarkStore` | Static utility for binary bookmark file I/O |
| `EpubReaderBookmarkListActivity` | Subactivity for browsing/deleting/jumping to bookmarks |
| `EpubReaderActivity` | Long-press Confirm calls `addBookmark()` directly |
| `EpubReaderMenuActivity` | "Save Passage" + "Bookmarks" menu items |
