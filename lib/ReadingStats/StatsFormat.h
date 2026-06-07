#pragma once
#include <cstdint>
#include <string>

namespace reading_stats {

// Format a millisecond duration for display. >= 1 hour -> "Xh Ym"; otherwise "Ym Zs".
std::string formatDurationMs(uint32_t ms);

// Derive a display name from a book file path: basename without the extension.
// "/books/great-gatsby.epub" -> "great-gatsby". A path with no '/' or no '.' is
// handled gracefully (returns the filename, or the whole string).
std::string pathToDisplayName(const std::string& path);

}  // namespace reading_stats
