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
  bool progressEstimated = false;
  bool progressBelowOnePercent = false;
  uint8_t progressPercent = 0;
  bool hasStartedReading = false;
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
  bool hasTodayReadingSeconds = false;
  uint32_t todayReadingSeconds = 0;
  uint32_t todayReadingSessions = 0;
  uint16_t dailyReadingGoalMinutes = 0;
  std::string chapterTitle;
};

enum class HomeBookAction : uint8_t { Open, Start, Continue, ReadAgain };

inline bool hasReliableHomeBookProgress(const HomeBookSummary& summary) {
  return summary.hasProgress && summary.progressState == DashboardMetricState::Available &&
         !summary.progressEstimated;
}

inline HomeBookAction homeBookAction(const HomeBookSummary& summary) {
  if (hasReliableHomeBookProgress(summary) && summary.progressPercent >= 100) return HomeBookAction::ReadAgain;
  if (summary.hasStartedReading) return HomeBookAction::Continue;
  const bool hasTrustedBookState =
      (summary.bookStatsState == DashboardMetricState::Available ||
       summary.bookStatsState == DashboardMetricState::NoData) &&
      (summary.progressState == DashboardMetricState::Available ||
       summary.progressState == DashboardMetricState::NoData);
  return hasTrustedBookState ? HomeBookAction::Start : HomeBookAction::Open;
}
