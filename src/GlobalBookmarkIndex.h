#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BookmarkStore.h"

// Aggregates bookmarks from every indexed book into a single queryable catalog.
// Persisted as /.crosspoint/global_bookmarks.bin; updated incrementally when
// per-book BookmarkStore instances save, and reconciled against the filesystem
// when GlobalBookmarksActivity opens.
class GlobalBookmarkIndex {
 public:
  struct Entry {
    std::string sourcePath;  // e.g. /books/foo.epub
    std::string cacheDir;    // e.g. /.crosspoint/epub_12345
    std::string title;       // display title
    bool isTxt = false;      // hint for jump dispatch
    std::vector<BookmarkStore::Bookmark> bookmarks;
  };

  // Singleton access.
  static GlobalBookmarkIndex& getInstance() { return instance; }

  // Persist/load the index to/from /.crosspoint/global_bookmarks.bin.
  void load();
  void save() const;

  // Per-book update. Replaces (or removes when empty) the entry for this source path.
  // Safe to call during BookmarkStore::save() hook.
  void upsertFromStore(const std::string& sourcePath, const std::string& cacheDir, const std::string& title, bool isTxt,
                       const std::vector<BookmarkStore::Bookmark>& bookmarks);

  // Drop an entry (e.g. when a book is deleted). No-op if not indexed.
  void removeBySourcePath(const std::string& sourcePath);

  // Convenience wrapper that pulls bookmarks out of a BookmarkStore and upserts.
  // Also removes the entry if the store is empty.
  void syncFromStore(const BookmarkStore& store, const std::string& sourcePath, const std::string& cacheDir,
                     const std::string& title, bool isTxt);

  // Walk every entry; stat sourcePath + cacheDir. Drop entries whose source file
  // is missing. Called on GlobalBookmarksActivity entry.
  // Returns true if anything changed (callers can decide whether to persist).
  bool reconcile();

  [[nodiscard]] const std::vector<Entry>& getEntries() const { return entries; }
  [[nodiscard]] bool isEmpty() const { return entries.empty(); }

  // Total bookmark count across all entries.
  [[nodiscard]] size_t totalBookmarkCount() const;

 private:
  static GlobalBookmarkIndex instance;

  std::vector<Entry> entries;
  bool loaded = false;

  static constexpr uint8_t FILE_VERSION = 1;
  static constexpr const char* FILE_PATH = "/.crosspoint/global_bookmarks.bin";

  std::vector<Entry>::iterator findBySourcePath(const std::string& sourcePath);
};

#define GLOBAL_BOOKMARKS GlobalBookmarkIndex::getInstance()
