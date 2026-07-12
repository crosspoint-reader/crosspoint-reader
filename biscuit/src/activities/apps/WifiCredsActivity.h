#pragma once
#include <string>
#include "activities/Activity.h"

class WifiCredsActivity final : public Activity {
 public:
  explicit WifiCredsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WifiCreds", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { INPUT_SSID, INPUT_PASS, SELECT_AUTH, DISPLAY_QR };
  State state = INPUT_SSID;

  std::string ssid;
  std::string password;
  int authType = 0;  // 0=WPA, 1=WEP, 2=Open

  void launchSsidKeyboard();
  void launchPassKeyboard();
  std::string buildWifiUri() const;
};
