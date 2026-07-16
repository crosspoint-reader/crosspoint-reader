#pragma once

#include <string>

// Moves a book file on the SD card without losing its reading state: the
// .crosspoint cache directory is keyed by a hash of the file path (see the
// Epub/Txt/Xtc constructors), so a plain rename would orphan the cache and
// with it the reading progress. moveFile re-keys the cache directory and
// repoints the Recent Books entry and the resume pointer alongside the file.
namespace BookMover {

// Cache directory for a book path, or "" for file types without a cache.
std::string cachePathFor(const std::string& path);

// Non-colliding destination inside dstDir (no trailing slash except root) for
// srcPath's filename: "name.epub", then "name (2).epub", "name (3).epub", ...
std::string buildDestination(const std::string& srcPath, const std::string& dstDir);

// Move srcPath to dstPath, migrating cache dir, Recent Books entry and the
// open-book resume pointer. Returns false when the file rename itself fails
// (everything left in place); a failed cache rename is non-fatal (the book
// moves, its progress is lost).
bool moveFile(const std::string& srcPath, const std::string& dstPath);

}  // namespace BookMover
