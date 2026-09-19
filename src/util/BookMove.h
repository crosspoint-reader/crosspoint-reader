#pragma once

#include <string>

// Moves a book file along with everything that tracks it by path: its cache
// directory, its recents entry, and the resume path. Leaves all of them alone
// when the file itself cannot be moved.
bool moveBook(const std::string& oldPath, const std::string& newPath);
