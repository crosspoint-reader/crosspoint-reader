#pragma once

#include <string>

// Migrates a book's reading data (metadata cache dir, bookmarks file, recent-books
// entry) from oldPath to newPath after the book file itself has already been
// renamed/moved to newPath on disk. Any stale cache/bookmarks already sitting at
// newPath are cleared first. Each step is best-effort and logged; returns true if
// the cache dir and bookmarks file (when present) were migrated successfully.
bool moveBookData(const std::string& oldPath, const std::string& newPath);
