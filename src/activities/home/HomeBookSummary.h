#pragma once

#include <cstdint>
#include <string>

#include "activities/home/DashboardStatsPolicy.h"

// Data prepared once by HomeActivity and consumed by themes. Themes must not
// perform SD-card reads while drawing; this keeps render latency predictable.
struct HomeBookSummary {
  DashboardMetricState bookStatsState = DashboardMetricState::NotTracked;
  DashboardMetricState progressState = DashboardMetricState::NotTracked;
  DashboardMetricState globalStatsState = DashboardMetricState::Unavailable;
  DashboardMetricState syncedStatsState = DashboardMetricState::NoData;
  bool usingSyncedStats = false;
  uint16_t syncedDeviceCount = 0;
  bool hasProgress = false;
  uint8_t progressPercent = 0;
  bool hasChapterPage = false;
  uint16_t chapterPageCurrent = 0;
  uint16_t chapterPageTotal = 0;
  uint32_t bookReadingSeconds = 0;
  uint32_t bookPagesTurned = 0;
  uint32_t bookSessions = 0;
  uint32_t globalReadingSeconds = 0;
  uint32_t globalPagesTurned = 0;
  bool hasCurrentStreak = false;
  uint16_t currentStreak = 0;
  std::string chapterTitle;
};
