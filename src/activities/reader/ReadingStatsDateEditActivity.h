#pragma once

#include <cstdint>
#include <string>

#include "BookReadingStats.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ReadingStatsDateEditActivity final : public Activity {
 public:
  static constexpr int ROW_COUNT = 7;

  ReadingStatsDateEditActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string cachePath,
                               BookReadingStats stats);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Page : uint8_t { Started, Finished };

  std::string cachePath;
  BookReadingStats stats;
  ReadingStatsDateTime fallbackDateTime;
  ButtonNavigator buttonNavigator;
  Page page = Page::Started;
  int selectedIndex = 0;
  bool saveFailed = false;

  ReadingStatsDate& currentDate();
  uint16_t& currentMinuteOfDay();
  bool& currentManualFlag();
  bool currentTimestampIsSet() const;
  void setCurrentTimestampEnabled(bool enabled);
  void openValueEditor(int row);
  void setEditedValue(int row, uint32_t value);
  bool timestampsAreOrdered() const;
  void advanceOrSave();
  std::string rowValue(int row) const;
};
