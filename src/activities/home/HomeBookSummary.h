#pragma once

#include <cstdint>
#include <string>

// Data prepared once by HomeActivity and consumed by themes. Themes must not
// perform SD-card reads while drawing; this keeps render latency predictable.
struct HomeBookSummary {
  bool hasProgress = false;
  uint8_t progressPercent = 0;
  uint32_t bookReadingSeconds = 0;
  uint32_t bookPagesTurned = 0;
  uint32_t bookSessions = 0;
  uint32_t estimatedTimeLeftSeconds = 0;
  uint32_t globalReadingSeconds = 0;
  uint32_t globalPagesTurned = 0;
  uint16_t currentStreak = 0;
  std::string chapterTitle;
};
