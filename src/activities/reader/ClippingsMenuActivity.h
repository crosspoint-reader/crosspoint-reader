#pragma once
#include <I18n.h>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class ClippingsMenuActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  static constexpr int ITEM_COUNT = 3;

  void toggleSetting(int idx);
  const char* settingValue(int idx) const;

 public:
  explicit ClippingsMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ClippingsMenu", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
};
