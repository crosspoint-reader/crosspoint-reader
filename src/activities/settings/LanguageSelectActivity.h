#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include <functional>

#include "../ActivityWithSubactivity.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;

/**
 * Activity for selecting UI language
 */
class LanguageSelectActivity final : public Activity {
 public:
  explicit LanguageSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  const std::function<void()>& onBack)
      : Activity("LanguageSelect", renderer, mappedInput), onBack(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  void handleSelection();

  std::function<void()> onBack;
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  constexpr static uint8_t totalItems = getLanguageCount();
};
