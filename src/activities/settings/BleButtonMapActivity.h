#pragma once

#include <I18n.h>

#include <cstdint>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Capture-then-assign mapping for BLE page-turner buttons. The user presses a
// button on the remote; we capture its decoded key identity (via the
// MappedInputManager BLE capture mode) and let them bind it to a logical button.
// Repeat to map each remote button; Back exits. Mirrors ButtonRemapActivity's
// flow, but the input source is the BLE host instead of the front buttons.
class BleButtonMapActivity final : public Activity {
 public:
  explicit BleButtonMapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BleButtonMap", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Logical functions a remote button can be bound to.
  struct Fn {
    MappedInputManager::Button button;
    StrId label;
  };
  static const Fn kFunctions[];
  static const uint8_t kFunctionCount;

  enum class Step { WaitForKey, SelectFunction };
  Step step = Step::WaitForKey;

  uint8_t capturedKind = 0xFF;
  uint8_t capturedValue = 0;
  int functionIndex = 0;

  ButtonNavigator buttonNavigator;

  // Bind the captured key to the chosen logical button in SETTINGS.bleKeyMap and
  // persist. Returns false when the table is full and the key is new.
  bool assignCapturedKey(MappedInputManager::Button button);
};
