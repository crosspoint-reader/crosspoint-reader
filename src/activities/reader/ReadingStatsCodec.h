#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

enum class ReadingStatsDecodeResult : uint8_t { Ok, Invalid, NewerFormat };

namespace ReadingStatsCodec {

using BookBytes = std::array<uint8_t, BookReadingStats::CURRENT_FILE_SIZE>;
using GlobalBytes = std::array<uint8_t, GlobalReadingStats::CURRENT_FILE_SIZE>;

BookBytes encode(const BookReadingStats& stats);
GlobalBytes encode(const GlobalReadingStats& stats);
ReadingStatsDecodeResult decode(const uint8_t* data, size_t size, BookReadingStats& stats);
ReadingStatsDecodeResult decode(const uint8_t* data, size_t size, GlobalReadingStats& stats);

}  // namespace ReadingStatsCodec
