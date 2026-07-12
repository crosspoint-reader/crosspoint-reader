#pragma once

#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class QrTotpActivity final : public Activity {
 public:
  explicit QrTotpActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("QrTotp", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == SHOW_QR; }

 private:
  enum State { SELECT_ACCOUNT, SHOW_QR };
  State state = SELECT_ACCOUNT;

  struct Account {
    char name[32];
    char secret[65];
    uint8_t digits;
    uint8_t period;
  };

  static constexpr int MAX_ACCOUNTS = 16;
  static constexpr const char* SAVE_PATH = "/biscuit/totp.dat";

  Account accounts[MAX_ACCOUNTS];
  int accountCount = 0;
  int selectedIndex = 0;

  // Last period index at which SHOW_QR was rendered — used for auto-refresh
  uint64_t lastPeriodIndex = 0;

  // True only when the system clock has been set via NTP (epoch > Sept 2020).
  // Without a battery-backed RTC the clock resets to 1970 on cold boot and
  // TOTP codes would be silently wrong.
  bool timeValid = false;

  void loadAccounts();

  static int base32Decode(const char* input, uint8_t* output, int outLen);
  static uint32_t generateTotp(const uint8_t* key, int keyLen, uint64_t counter, int digits);
  uint32_t currentCode() const;
};
