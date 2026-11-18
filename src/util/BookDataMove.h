#pragma once

#include <string>

// Removes a book's reading cache and bookmarks file, if any, keeping a bookmarks file shared
// with sourcePath (getBookmarkPath() ignores the extension).
void clearBookData(const std::string& path, const std::string& sourcePath);

// Called after the book file itself has been moved successfully. Migrates the book's
// reading data (metadata cache dir, bookmarks file, recent-books entry) from oldPath to
// newPath, clearing any stale data already sitting at newPath. Best effort: each step is
// independent and failures are only logged; the recent-books entry follows newPath, or is
// dropped when newPath is not a supported book type.
void moveBookData(const std::string& oldPath, const std::string& newPath);
