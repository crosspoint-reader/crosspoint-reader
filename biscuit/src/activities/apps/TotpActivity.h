#pragma once
#include <cstdint>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class TotpActivity final : public Activity {
 public:
  explicit TotpActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Totp", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { ACCOUNT_LIST, SHOW_CODE, ADD_ACCOUNT };
  State state = ACCOUNT_LIST;

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

  // For ADD_ACCOUNT flow
  char pendingName[32] = {};

  void loadAccounts();
  void saveAccounts();
  void renderList() const;
  void renderCode() const;

  static int base32Decode(const char* input, uint8_t* output, int outLen);
  static uint32_t generateTotp(const uint8_t* key, int keyLen, uint64_t counter, int digits);
  uint32_t currentCode() const;
};
