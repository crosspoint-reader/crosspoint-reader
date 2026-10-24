#pragma once

#include <string>

#include "ReaderFont.h"

// A book's own reader font, kept in its cache directory so it moves with the
// book (rename, move to /Read). Clearing all reading caches drops it, like
// reading progress.
namespace BookFontFile {

// False when the book has no font file or it cannot be read.
bool load(const std::string& cacheDir, ReaderFont& font);
bool save(const std::string& cacheDir, const ReaderFont& font);
void remove(const std::string& cacheDir);

}  // namespace BookFontFile
