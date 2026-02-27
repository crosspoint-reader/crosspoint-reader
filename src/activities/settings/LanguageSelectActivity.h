#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include <functional>

#include "../Activity.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;

/**
 * Activity for selecting UI language
 */
class LanguageSelectActivity final : public Activity {
 public:
  explicit LanguageSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LanguageSelect", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void handleSelection();

  void onBack() { finish(); }
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  constexpr static uint8_t totalItems = getLanguageCount();
};
