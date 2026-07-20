#pragma once

#include <I18n.h>

#include <array>
#include <string>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ReadingStatsActivity final : public Activity {
 public:
  ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookTitle,
                       const BookReadingStats& bookStats, const GlobalReadingStats& globalStats);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Row {
    StrId label;
    std::string value;
  };

  std::string bookTitle;
  std::array<Row, 7> rows;
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
};
