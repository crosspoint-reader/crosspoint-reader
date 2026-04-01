#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include "../Activity.h"
#include "DictionaryManager.h"
#include "LookupHistory.h"
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
  // The first item in the list is "Lookup History" (not a dictionary).
  // Dictionary indices are offset by 1 in the list.
  static constexpr int HISTORY_ITEM_INDEX = 0;
  int dictListOffset() const { return 1; }
};
