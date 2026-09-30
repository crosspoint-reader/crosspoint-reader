#pragma once

#include "HomeButtonSettings.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class HomeButtonSettingsActivity final : public UiListActivity {
 public:
  HomeButtonSettingsActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("HomeButtonSettings", renderer, input) {}

 private:
  static constexpr int GESTURE_COUNT = 3;

  freeink::ui::ListItem rows[GESTURE_COUNT]{};
  OptionPopup optionPopup;

  int listCount() const override { return GESTURE_COUNT; }
  const char* headerTitle() const override { return tr(STR_HOME_BUTTON); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void render(RenderLock&&) override;
};
