#pragma once

#include <string>

// Removes a book's cache and bookmarks, keeping bookmarks shared with sourcePath. Only EPUB/TXT/MD
// have bookmarks, named by path without extension and compared ignoring case (FAT).
void clearBookData(const std::string& path, const std::string& sourcePath);

// After a successful file move, moves the book's cache, bookmarks and recent entry to newPath.
void moveBookData(const std::string& oldPath, const std::string& newPath);
