#pragma once
#include <string>

#include "../Activity.h"
#include "BookReadingStats.h"

class BookStatsActivity final : public Activity {
  std::string bookTitle;
  BookReadingStats stats;

 public:
  BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                    const BookReadingStats& stats);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
