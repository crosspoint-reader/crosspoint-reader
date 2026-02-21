#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include <functional>

#include "../Activity.h"
#include "components/UITheme.h"

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
  void render(RenderLock&&) override;

 private:
  void handleSelection();

  std::function<void()> onBack;
  int selectedIndex = 0;
  int totalItems = 0;
};
