#pragma once

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class AdvancedSettingsActivity final : public UiListActivity {
 public:
  AdvancedSettingsActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("AdvancedSettings", renderer, input) {}

 private:
  freeink::ui::ListItem row{};
  OptionPopup optionPopup;

  int listCount() const override { return 1; }
  const char* headerTitle() const override { return tr(STR_ADVANCED); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void render(RenderLock&&) override;

  void setPluginSystemEnabled(bool enabled);
};
