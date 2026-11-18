#pragma once

#include <string>

// Removes a book's reading cache and bookmarks file, if any, keeping a bookmarks file shared
// with sourcePath (getBookmarkPath() ignores the extension and FAT ignores case). Does nothing
// for folders and files that are not supported books.
void clearBookData(const std::string& path, const std::string& sourcePath);

// Called after the book file itself has been moved successfully. Migrates the book's
// reading data (metadata cache dir, bookmarks file, recent-books entry) from oldPath to
// newPath, clearing any stale data already sitting at newPath. Best effort: each step is
// independent and failures are only logged. Folders and non-book files have no data to
// migrate; when newPath is not a supported book, the old data is dropped instead.
void moveBookData(const std::string& oldPath, const std::string& newPath);
