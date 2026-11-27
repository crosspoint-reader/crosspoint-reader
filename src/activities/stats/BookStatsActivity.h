#pragma once

#include <cstdint>
#include <string>

#include "activities/UiListActivity.h"

// One book's reading stats, opened from the reader menu. The last row toggles
// the book's finished state.
class BookStatsActivity final : public UiListActivity {
 public:
  // liveSeconds and livePages are the open reading session, which is saved
  // only when it ends; it is applied to the saved stats for display.
  explicit BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                             uint32_t liveSeconds, uint32_t livePages);

  static constexpr int ROW_COUNT = 6;

  void onEnter() override;

 private:
  static constexpr int FINISHED_ROW = ROW_COUNT - 1;

  int listCount() const override { return ROW_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  const std::string bookPath;
  const uint32_t liveSeconds;
  const uint32_t livePages;
  bool finished = false;
  std::string rowValues[FINISHED_ROW];
  freeink::ui::ListItem rowItems[ROW_COUNT]{};
};
