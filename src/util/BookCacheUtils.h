#pragma once

#include <string>

// Returns the cache directory a book file maps to, or an empty string if the
// extension is not a recognised book type (EPUB, XTC, or TXT). The mapping is
// derived from the full path, so moving a book changes its cache directory.
std::string bookCachePath(const std::string& path);

// Clears the reading cache for a book file if its extension is recognised
// (EPUB, XTC, or TXT). Does nothing for other file types.
void clearBookCache(const std::string& path);

// Follows a book's cache directory after its backing file moved from oldPath to
// newPath, keeping reading progress, and repoints the recents entry and the
// resume path. Call once the file itself has been moved on disk. Drops the
// cache instead when the destination is not a recognised book type.
void moveBookCache(const std::string& oldPath, const std::string& newPath);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);
