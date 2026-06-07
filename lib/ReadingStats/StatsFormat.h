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

// Reading speed in pages per hour (0 if no time). uint64 intermediate to avoid overflow.
uint32_t pagesPerHour(uint32_t pages, uint32_t totalMs);

// Average milliseconds spent per page (0 if no pages).
uint32_t avgMsPerPage(uint32_t totalMs, uint32_t pages);

// Average milliseconds per reading session (0 if no sessions).
uint32_t avgMsPerSession(uint32_t totalMs, uint32_t sessions);

}  // namespace reading_stats
