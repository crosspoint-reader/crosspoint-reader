#pragma once

#include "activities/Activity.h"

class LockSettingsActivity final : public Activity {
 public:
  explicit LockSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LockSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Mode { MENU, SETUP, CONFIRM };

  Mode mode = Mode::MENU;
  int menuSelection = 0;

  // Code entry state
  uint8_t newCode[6] = {};
  uint8_t newCodeLength = 0;
  uint8_t confirmCode[6] = {};
  uint8_t confirmCodeLength = 0;

  unsigned long messageUntil = 0;
  const char* messageText = nullptr;

  void showMessage(const char* text, unsigned long durationMs = 1500);
  void handleMenuConfirm();
  void handleCodeInput(int pressed);
  void handleConfirmInput(int pressed);
  int getPressedButton();
};
