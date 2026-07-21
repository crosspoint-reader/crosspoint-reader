#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace DashboardProgress {

struct Position {
  uint16_t spineIndex = 0;
  uint16_t pageNumber = 0;
  uint16_t pageCount = 0;
};

inline bool decode(const uint8_t* data, const size_t size, Position& position) {
  position = {};
  if (!data || size != 6) return false;

  position.spineIndex = static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1]) << 8;
  position.pageNumber = static_cast<uint16_t>(data[2]) | static_cast<uint16_t>(data[3]) << 8;
  position.pageCount = static_cast<uint16_t>(data[4]) | static_cast<uint16_t>(data[5]) << 8;
  return position.pageCount > 0 && position.pageNumber != UINT16_MAX && position.pageNumber < position.pageCount;
}

inline bool validate(const Position& position, const int spineCount, const std::optional<uint16_t> finalizedPageCount) {
  return spineCount > 0 && position.spineIndex < spineCount && finalizedPageCount.has_value() &&
         *finalizedPageCount == position.pageCount;
}

inline bool toPercent(const float progress, uint8_t& percent) {
  percent = 0;
  if (!std::isfinite(progress)) return false;
  percent = static_cast<uint8_t>(std::lround(std::clamp(progress, 0.0F, 1.0F) * 100.0F));
  return true;
}

inline bool fromCompletedStats(const bool trusted, const bool completed, uint8_t& percent) {
  if (!trusted || !completed) return false;
  percent = 100;
  return true;
}

inline int fillWidth(const int barWidth, const uint8_t percent) {
  if (barWidth <= 4) return 0;
  return (barWidth - 4) * std::min<unsigned>(percent, 100U) / 100;
}

}  // namespace DashboardProgress
