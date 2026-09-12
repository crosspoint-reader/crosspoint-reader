#pragma once

#include <string>

// Empty when the extension is not a recognised book type (EPUB, XTC, or TXT).
std::string bookCachePath(const std::string& path);

// Clears the reading cache for a book file if its extension is recognised
// (EPUB, XTC, or TXT). Does nothing for other file types.
void clearBookCache(const std::string& path);

// Where a book's cache sits after a move. `to` is the directory the cache
// occupies now, which stays equal to `from` when it could not be moved, so
// callers that store paths into the cache keep pointing at real files.
struct BookCacheMove {
  std::string from;
  std::string to;
  bool dropped = false;
};

// Removes the cache directory a file would map to, if one is there. A book
// about to be moved onto that path must not inherit it. False when it is
// there and could not be removed.
bool clearCacheAt(const std::string& path);

// Call once the file itself has been moved on disk, and once the destination
// has been cleared with clearCacheAt: a cache still sitting there fails the
// move, since SdFat's rename does not overwrite. Drops the cache instead of
// moving it when the destination is not a recognised book type.
BookCacheMove moveBookCache(const std::string& oldPath, const std::string& newPath);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);
