#include "StatsFormat.h"

#include <I18n.h>

#include <cstdio>

namespace statsformat {

std::string duration(const uint32_t seconds) {
  const uint32_t minutes = seconds / 60;
  char buf[32];
  if (minutes >= 60) {
    snprintf(buf, sizeof(buf), tr(STR_STATS_HOURS_MINUTES), static_cast<unsigned>(minutes / 60),
             static_cast<unsigned>(minutes % 60));
  } else {
    snprintf(buf, sizeof(buf), tr(STR_STATS_MINUTES), static_cast<unsigned>(minutes));
  }
  return buf;
}

std::string averageSession(const uint32_t sessionSeconds, const uint32_t sessions) {
  return sessions == 0 ? std::string("-") : duration(sessionSeconds / sessions);
}

std::string pagesPerMinute(const uint32_t pages, const uint32_t seconds) {
  if (seconds < 60) return "-";
  // Tenths of a page per minute, rounded, in 64 bits so large totals cannot overflow.
  const uint64_t tenths = (static_cast<uint64_t>(pages) * 600 + seconds / 2) / seconds;
  char buf[24];
  snprintf(buf, sizeof(buf), "%llu.%llu", static_cast<unsigned long long>(tenths / 10),
           static_cast<unsigned long long>(tenths % 10));
  return buf;
}

std::string today(const uint32_t seconds, const uint8_t goalMinutes) {
  if (goalMinutes == 0) return duration(seconds);
  char buf[32];
  snprintf(buf, sizeof(buf), tr(STR_STATS_GOAL_PROGRESS), static_cast<unsigned>(seconds / 60),
           static_cast<unsigned>(goalMinutes));
  return buf;
}

std::string count(const uint32_t value) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(value));
  return buf;
}

}  // namespace statsformat
