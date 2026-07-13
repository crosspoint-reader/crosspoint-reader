#pragma once

#include <BootSwitch.h>

#include "activities/Activity.h"

/**
 * Dual-OS slot switch activity ("Switch OS").
 *
 * Flow:
 *  1) onEnter -> peek the passive OTA slot; if it holds no app image, show
 *     "No other OS installed".
 *  2) Otherwise push ConfirmationActivity ("Restart into other OS?").
 *  3) On confirm: repoint otadata at the passive slot (boot_switch::swapToPassive)
 *     and restart into the other firmware.
 *
 * Reached from Settings -> System -> "Switch OS", or by holding the upper side
 * button while powering on (bootMode) — see main.cpp. The boot-hold shortcut is
 * app-level convenience, NOT a recovery path: it only runs when this slot boots.
 */
class SwapBootSlotActivity : public Activity {
 public:
  enum class State {
    CONFIRMING,
    NO_TARGET,
    SWAPPING,
    FAILED,
  };

  explicit SwapBootSlotActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool bootMode = false)
      : Activity("SwapBootSlot", renderer, mappedInput), bootMode(bootMode) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::SWAPPING; }

 private:
  State state = State::CONFIRMING;
  bool bootMode = false;
  boot_switch::PassiveSlotInfo info = {};

  void onConfirmationResult(const ActivityResult& result);
  void dismiss();
};
