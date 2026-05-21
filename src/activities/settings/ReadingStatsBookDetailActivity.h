#pragma once

#include <string>

#include "activities/Activity.h"

// Phase-2 sub-screen: per-book reading stats with a 30-day sparkline keyed on
// that book's own day buckets. Constructed with the docId of the book to
// display; resolved against ReadingStatsStore on each render so a session
// that finishes while this screen is open updates the next time we redraw.
class ReadingStatsBookDetailActivity final : public Activity {
 public:
  ReadingStatsBookDetailActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string docId)
      : Activity("ReadingStatsBookDetail", renderer, mappedInput), docId(std::move(docId)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string docId;
};
