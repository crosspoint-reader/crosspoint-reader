#pragma once

#include "activities/Activity.h"
#include "components/OptionPopup.h"

class ResetActivity final : public Activity {
 public:
  enum class Mode { SettingsOnly, Factory };

  explicit ResetActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Mode mode)
      : Activity("Reset", renderer, mappedInput), mode(mode) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  const Mode mode;
  OptionPopup confirmPopup;
  void showConfirmation(bool confirmAgain);
  void resetAndRestart();
};
