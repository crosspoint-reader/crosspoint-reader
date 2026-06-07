#pragma once
#include <ReadingStats.h>

#include "activities/Activity.h"

// Detail page for a single book's reading statistics: full metrics plus a
// comparative bar of this book's speed vs. the all-books average. Read-only;
// Back (finish) returns to the statistics list.
class ReadingStatsDetailActivity final : public Activity {
  reading_stats::BookStats book;
  uint32_t globalPages;
  uint32_t globalMs;

 public:
  ReadingStatsDetailActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, reading_stats::BookStats book,
                             uint32_t globalPages, uint32_t globalMs)
      : Activity("ReadingStatsDetail", renderer, mappedInput),
        book(std::move(book)),
        globalPages(globalPages),
        globalMs(globalMs) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
