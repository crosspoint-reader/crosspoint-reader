#pragma once

#include "activities/Activity.h"
#include "components/OptionPopup.h"

class ResetActivity final : public Activity {
 public:
  explicit ResetActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Reset", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  OptionPopup confirmPopup;
  void showConfirmation(bool confirmAgain);
  void resetAndRestart();
};
