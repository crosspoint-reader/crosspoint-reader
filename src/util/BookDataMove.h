#pragma once

#include <string>

// Called after the book file itself has been moved successfully. Migrates the book's
// reading data (metadata cache dir, bookmarks file, recent-books entry) from oldPath to
// newPath. Any stale cache/bookmarks already sitting at newPath are cleared first.
// Best effort: each step is independent and failures are only logged. The recent-books
// entry always follows the new path.
void moveBookData(const std::string& oldPath, const std::string& newPath);
