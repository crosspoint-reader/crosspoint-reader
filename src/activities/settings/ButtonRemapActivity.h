#pragma once

#include <functional>
#include <string>

#include "activities/Activity.h"

class ButtonRemapActivity final : public Activity {
 public:
  explicit ButtonRemapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const std::function<void()>& onBack)
      : Activity("ButtonRemap", renderer, mappedInput), onBack(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  // Rendering task state.

  // Callback used to exit the remap flow back to the settings list.
  const std::function<void()> onBack;
  // Index of the logical role currently awaiting input.
  uint8_t currentStep = 0;
  // Temporary mapping from logical role -> hardware button index.
  uint8_t tempMapping[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  // Error banner timing (used when reassigning duplicate buttons).
  unsigned long errorUntil = 0;
  std::string errorMessage;

  // Commit temporary mapping to settings.
  void applyTempMapping();
  // Returns false if a hardware button is already assigned to a different role.
  bool validateUnassigned(uint8_t pressedButton);
  // Labels for UI display.
  const char* getRoleName(uint8_t roleIndex) const;
  const char* getHardwareName(uint8_t buttonIndex) const;
};
