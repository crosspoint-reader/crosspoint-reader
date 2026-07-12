#pragma once
#include <cstdint>
#include "activities/Activity.h"

class SecurityPinActivity final : public Activity {
 public:
  explicit SecurityPinActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SecurityPin", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Check if PIN protection is enabled (reads SD card)
  static bool isPinEnabled();

 private:
  enum State { ENTER_PIN, SET_PIN, SET_DURESS_PIN, SETTINGS_MENU };
  State state = ENTER_PIN;

  // PIN entry
  char pinBuffer[7] = {};  // Max 6 digits + null
  int pinPos = 0;           // Current digit position
  int pinLength = 4;        // Total PIN length being entered (4-6)

  // Stored hashes
  uint8_t storedPinHash[32] = {};
  uint8_t storedDuressHash[32] = {};
  uint8_t flags = 0;  // bit0=pin enabled, bit1=duress enabled, bit2=auto-wipe on 5 fails
  int failCount = 0;
  static constexpr int MAX_FAILS = 5;

  // Settings menu
  int menuIndex = 0;
  static constexpr int MENU_ITEMS = 5;  // Change PIN, Set Duress PIN, Toggle Auto-Wipe, Disable PIN, Back

  static constexpr const char* PIN_PATH = "/biscuit/security.dat";

  // New PIN being set
  char newPin[7] = {};
  bool settingFirstEntry = true;  // true=first entry, false=confirm entry

  // Message display timing
  unsigned long msgUntilMs = 0;
  char msgBuf[32] = {};

  void loadConfig();
  void saveConfig();
  static void hashPin(const char* pin, uint8_t* out);
  bool checkPin(const char* pin, const uint8_t* hash) const;
  void handlePinEntry();
  void handleSetPin();
  void handleSettingsMenu();
  void clearPinBuffer();
  void showMessage(const char* msg, unsigned long durationMs = 1200);

  void renderPinEntry() const;
  void renderSetPin() const;
  void renderSettingsMenu() const;
  void renderPinBoxes(int y, int length, int activePos, const char* digits) const;
};
