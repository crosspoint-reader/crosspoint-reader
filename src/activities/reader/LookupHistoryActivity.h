#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include "../Activity.h"
#include "DictionaryManager.h"
#include "LookupHistory.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;

class LookupHistoryActivity final : public Activity {
 public:
  explicit LookupHistoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LookupHistory", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void lookupWord(int index);
  void confirmDeleteEntry(int index);
  void confirmClearAll();

  static constexpr unsigned long LONG_PRESS_MS = 1000;

  ButtonNavigator buttonNavigator;
  DictionaryManager dictManager;
  LookupHistory history;
  int selectedIndex = 0;
  bool ignoreNextConfirmRelease = true;  // Absorb stale release from launching activity
  bool longPressConsumed = false;        // Prevent short-press action after long-press fires
};
