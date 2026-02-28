#pragma once
#include <cstdint>

#include "../Activity.h"
#include "ReadingStats.h"

class ReaderStatsActivity final : public Activity {
  const BookStats bookStats;
  const GlobalStats globalStats;
  const uint32_t sessionSeconds;
  const uint32_t sessionPages;
  const uint32_t sessionWords;

 public:
  ReaderStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const BookStats& bookStats,
                      const GlobalStats& globalStats, uint32_t sessionSeconds, uint32_t sessionPages,
                      uint32_t sessionWords)
      : Activity("ReaderStats", renderer, mappedInput),
        bookStats(bookStats),
        globalStats(globalStats),
        sessionSeconds(sessionSeconds),
        sessionPages(sessionPages),
        sessionWords(sessionWords) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
