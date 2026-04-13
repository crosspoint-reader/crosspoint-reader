#pragma once

#include <cstdint>
#include <string>

struct BookReadingStats {
  uint32_t totalSecondsRead = 0;
  uint8_t lastProgressPercent = 0;  // 0-100
};

namespace ReadingStatsStore {

// Returns the .crosspoint cache directory path for a book file path
std::string getCachePath(const std::string& bookPath);

// Load reading stats for a book from its stats.bin file.
// Returns zeroed defaults if the file does not exist or cannot be read.
BookReadingStats load(const std::string& bookPath);

// Save reading stats for a book to its stats.bin file.
// Returns true on success.
bool save(const std::string& bookPath, const BookReadingStats& stats);

// Format seconds as "Xh Ym" (if >= 1h) or "Ym" (if < 1h).
std::string formatTime(uint32_t seconds);

// Estimate remaining seconds given time already spent and current progress (0-100).
// Returns 0 if progress is 0 or >= 100.
uint32_t estimateSecondsLeft(uint32_t secondsRead, uint8_t progressPercent);

}  // namespace ReadingStatsStore
