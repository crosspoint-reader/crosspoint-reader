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

// Call once the file itself has been moved on disk. Drops the cache instead of
// moving it when the destination is not a recognised book type.
BookCacheMove moveBookCache(const std::string& oldPath, const std::string& newPath);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);
