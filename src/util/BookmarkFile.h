#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../BookmarkEntry.h"

// Per-book bookmark persistence. Takes the book's path; bookmark-file path
// derivation (BookmarkUtil) and directory creation are hidden inside.
namespace BookmarkFile {

// Loads the bookmarks for bookPath. The vector is cleared first; a missing or
// empty bookmark file yields an empty list and returns false.
// Sync bookkeeping stored beside the bookmarks.
struct SyncState {
  std::vector<uint64_t> deleted;  // sync ids of removed bookmarks the server has not been told about
  std::string account;            // account the synced flags and deletions belong to
};

bool load(const std::string& bookPath, std::vector<BookmarkEntry>& bookmarks, SyncState* state = nullptr);

// Saves the bookmarks for bookPath, creating the bookmarks directory as needed.
// Once the book has been synced, a bookmark missing from the list is recorded as a pending deletion.
bool save(const std::string& bookPath, const std::vector<BookmarkEntry>& bookmarks);

// Saves the list a sync with account produced and clears the pending deletions.
bool saveSynced(const std::string& bookPath, const std::vector<BookmarkEntry>& bookmarks, const std::string& account);

}  // namespace BookmarkFile
