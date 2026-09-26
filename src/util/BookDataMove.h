#pragma once

#include <string>

// Removes a book's reading cache and bookmarks file, if any. Does nothing for unsupported types.
void clearBookData(const std::string& path);

// Called after the book file itself has been moved successfully. Migrates the book's
// reading data (metadata cache dir, bookmarks file, recent-books entry) from oldPath to
// newPath, clearing any stale data already sitting at newPath. Best effort: each step is
// independent and failures are only logged; the recent-books entry always follows newPath.
void moveBookData(const std::string& oldPath, const std::string& newPath);
