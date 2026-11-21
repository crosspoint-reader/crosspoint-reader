#pragma once

#include <string>

#include "ReaderFont.h"

// A book's own reader font and spacing, kept in its cache directory so it
// moves with the book (rename, move to /Read). Clearing all reading caches
// drops it, like reading progress.
namespace BookFontFile {

// Leaves `book` with no fields when the book has no font file or it cannot be
// read. Values out of range are skipped.
void load(const std::string& cacheDir, BookFont& book);
bool save(const std::string& cacheDir, const BookFont& book);
// True when the book has no font file afterwards.
bool remove(const std::string& cacheDir);

}  // namespace BookFontFile
