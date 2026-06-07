#pragma once
#include <ReadingStats.h>

#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Top-level screen showing all-time reading totals and a scrollable per-book list
// (pages, time, reading speed). Read-only; Back returns Home.
class ReadingStatsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  size_t selectorIndex = 0;
  std::vector<reading_stats::BookStats> books;  // snapshot, sorted by time desc
  uint32_t totalPages = 0;
  uint32_t totalMs = 0;

  void loadStats();

 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
