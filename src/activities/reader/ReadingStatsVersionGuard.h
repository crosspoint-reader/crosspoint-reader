#pragma once

#include <cstdint>

namespace ReadingStatsVersionGuard {

enum class Result : uint8_t { NoNewerFile, NewerFile, IoError };

// Scans one directory for <prefix><version>.bin[.bak|.tmp]. This keeps an
// older firmware from loading or shadowing a sibling written by a newer one.
Result scan(const char* directoryPath, const char* fileNamePrefix, uint16_t currentVersion);

}  // namespace ReadingStatsVersionGuard
