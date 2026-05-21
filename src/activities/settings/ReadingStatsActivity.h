#pragma once

#include "activities/Activity.h"

// Phase-1 reading-stats screen. Read-only summary of the per-book and global
// reading counters collected by ReadingSessionTracker. Mirrors the layout of
// SystemInformationActivity so it slots into the existing settings flow with
// no new theme work. Future phases will add per-book drill-in, day-by-day
// sparklines, and a web-frontend dashboard.
class ReadingStatsActivity final : public Activity {
 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Last millis() at which we refreshed the live "this session" block.
  // We only redraw once per second to avoid hammering the e-ink panel.
  uint32_t lastLiveRefreshMs = 0;
};
