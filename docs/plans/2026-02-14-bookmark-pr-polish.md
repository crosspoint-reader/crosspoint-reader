# Bookmark PR #781 Polish Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Address PR review feedback — add delete hint to bookmark list and fix filename collision via path hashing.

**Architecture:** Two isolated changes: (1) UI hint text in bookmark list renderer, (2) hash-based bookmark filename in BookmarkStore. No migration needed — bookmarks are a new unreleased feature.

**Tech Stack:** C++20 on ESP32 (PlatformIO/Arduino), e-ink display, FreeRTOS

---

## Task 1: Add "hold to delete" hint in bookmark list

**Files:**
- Modify: `src/activities/reader/EpubReaderBookmarkListActivity.cpp:154-157`

**Context:** osteotek requested a hint that holding confirm deletes a bookmark. The reader menu shows a subtitle at y=45 (progress summary). The bookmark list title is at y=15+contentY, and list items start at y=60+contentY. There's a 45px gap between title and list — room for a centered subtitle at y=40+contentY.

**Step 1: Add hint subtitle below "Bookmarks" title**

In `renderScreen()`, after drawing the title (line 157), add a centered hint line. Only show it when not in delete-confirmation mode and there are bookmarks:

```cpp
  // After line 157 (title drawing):
  if (!confirmingDelete && totalItems > 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, 40 + contentY, "Hold confirm to delete");
  }
```

This mirrors the pattern from `EpubReaderMenuActivity.cpp:116` which draws a centered subtitle at y=45.

**Step 2: Build and verify**

Run: `pio run -e default`
Expected: Clean build, no errors.

**Step 3: Commit**

```bash
git add src/activities/reader/EpubReaderBookmarkListActivity.cpp
git commit -m "feat: add hold-to-delete hint in bookmark list"
```

---

## Task 2: Hash book path for bookmark filename

**Files:**
- Modify: `src/BookmarkStore.cpp:7-23` (replace `getBookmarkPath`)
- Modify: `src/BookmarkStore.h:31` (update private method signature — no change needed, just note it stays same)

**Context:** Current implementation uses only the book's basename, so two books with the same filename in different folders collide. Replace with FNV-1a hash of the full path, formatted as 8-char hex. No external libraries needed — FNV-1a is a simple inline loop.

**Step 1: Replace `getBookmarkPath` with hash-based implementation**

Replace the entire `getBookmarkPath` function in `BookmarkStore.cpp` (lines 7-23):

```cpp
std::string BookmarkStore::getBookmarkPath(const std::string& bookPath) {
  // FNV-1a hash of full book path to avoid filename collisions
  uint32_t hash = 2166136261u;
  for (const char c : bookPath) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  char hexHash[9];
  snprintf(hexHash, sizeof(hexHash), "%08x", hash);
  return std::string(BOOKMARKS_DIR) + "/" + hexHash + ".bookmarks";
}
```

This produces deterministic 8-char hex filenames like `/.crosspoint/bookmarks/a1b2c3d4.bookmarks`. FNV-1a is well-suited for short strings and has good distribution.

**Step 2: Update the header comment**

In `BookmarkStore.h`, update the file path comment on line 15 to reflect the new naming:

```cpp
// Files are stored at /.crosspoint/bookmarks/<path-hash>.bookmarks
```

**Step 3: Build and verify**

Run: `pio run -e default`
Expected: Clean build, no errors.

**Step 4: Commit**

```bash
git add src/BookmarkStore.cpp src/BookmarkStore.h
git commit -m "fix: use path hash for bookmark filenames to avoid collisions"
```

---

## Task 3: Reply to PR review comments

**After both code changes are pushed**, reply to the review threads:

1. **Copilot's filename collision comment** (`BookmarkStore.cpp:14`): Explain we now hash the full path with FNV-1a.

2. **osteotek's agreement** on the collision comment: Acknowledge, done.

3. **daveallie's question** about storage location (`BookmarkStore.h:17`): Explain bookmarks are intentionally stored separately from cache — bookmarks are user data that should survive cache clearing. The filename collision issue is now resolved via path hashing.

4. **pablohc/osteotek cache clearing discussion** (`BookmarkStore.h:17`): Acknowledge the valid concern. Bookmarks live outside the cache directory so they survive cache clearing by design. A separate "clear bookmarks" option could be a follow-up if needed.

5. **osteotek's delete hint request** (PR comment): Done, added subtitle hint.

6. **Copilot's race condition comments** (EpubReaderActivity.cpp:162, EpubReaderBookmarkListActivity.cpp:80): Evaluate and note whether these are real concerns or false positives given the FreeRTOS task model. If they're valid, file as follow-up issues rather than blocking this PR.
