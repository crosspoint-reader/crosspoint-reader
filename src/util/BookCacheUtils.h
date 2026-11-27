#pragma once

#include <string>

// Clears the reading cache for a book file if its extension is recognised
// (EPUB, XTC, or TXT). Does nothing for other file types.
void clearBookCache(const std::string& path);

// Call after the file at `path` is replaced or removed. If it is a custom sleep
// image (/sleep.bmp, or a file in /sleep or /.sleep), removes the converted
// copies in /.crosspoint so the next sleep converts it again.
void sleepImageChanged(const char* path);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);

// Delete a local book and its local clipping state after the file is removed.
bool removeBookFile(const std::string& path);

// Moves a book file together with its reading state (layout cache and progress,
// bookmarks, clippings), and points the recent-books list and the last open book at
// the new path. False when it could not, with the book and its state left in place.
bool moveBookFile(const std::string& from, const std::string& to);
