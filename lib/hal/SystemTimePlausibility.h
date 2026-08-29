#pragma once

#include <ctime>

// Threshold below which a system clock reading cannot be a real calendar date.
// 2024-01-01T00:00:00Z: comfortably after the post-boot default (epoch 0) and
// any unsynced-clock artifact, comfortably before any plausible present day.
constexpr time_t MIN_PLAUSIBLE_EPOCH = 1704067200;

// Pure predicate behind HalClock::isSystemTimeValid(), split out so the rule can
// be exercised on the host without the RTC/SNTP dependencies HalClock.h carries.
inline bool isPlausibleEpoch(const time_t epochSeconds) { return epochSeconds >= MIN_PLAUSIBLE_EPOCH; }
