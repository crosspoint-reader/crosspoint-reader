#pragma once
#include <I18n.h>

#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/UiListActivity.h"

// Full-screen list of the day's half-hour slots for a night-schedule time; the
// option popup cannot scroll 48 rows. Selecting a slot persists it and returns.
class NightTimePickerActivity final : public UiListActivity {
 public:
  NightTimePickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, StrId titleId,
                          uint8_t CrossPointSettings::* field);

  void onEnter() override;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  StrId titleId_;
  uint8_t CrossPointSettings::* field_;
  std::vector<std::string> labels_;
  std::vector<freeink::ui::ListItem> rowItems_;
};
