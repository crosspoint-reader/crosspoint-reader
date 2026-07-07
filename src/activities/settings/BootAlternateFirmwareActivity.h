#pragma once

#include <esp_app_desc.h>

#include "activities/Activity.h"

// Allows switching the active boot partition to whichever OTA slot is not
// currently running, provided it contains a valid firmware image. Shows the
// detected firmware name and version before committing.
class BootAlternateFirmwareActivity final : public Activity {
 public:
  explicit BootAlternateFirmwareActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BootAlternateFirmware", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State { CONFIRM, SWITCHING, FAILED };

  State state = State::CONFIRM;
  esp_app_desc_t altDesc = {};
  bool altIsCrossPoint = false;

  void doSwitch();
};
