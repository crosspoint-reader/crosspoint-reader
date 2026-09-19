#pragma once

#include <string>
#include <vector>

namespace NextBookFinder {

// Collects up to maxCount book files that order after currentBookPath's filename
// (natural sort, same ordering as the file browser) within the same folder.
// Returns bare filenames in sorted order; the current file itself is excluded.
// Single directory pass keeping only the maxCount best matches, so memory stays
// bounded regardless of folder size.
std::vector<std::string> findNextBooks(const std::string& currentBookPath, size_t maxCount);

}  // namespace NextBookFinder
