#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include "../Activity.h"
#include "DictionaryManager.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;

class DictionarySettingsActivity final : public Activity {
 public:
  explicit DictionarySettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DictionarySettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void toggleSelected();

  ButtonNavigator buttonNavigator;
  DictionaryManager dictManager;
  int selectedIndex = 0;
};
