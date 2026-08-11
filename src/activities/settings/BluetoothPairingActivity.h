#pragma once

#include <BleKeyboardHost.h>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class BluetoothPairingActivity final : public Activity {
 public:
  explicit BluetoothPairingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BluetoothPairing", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  enum class State { Starting, Scanning, Connecting, Connected, Error };

  State state_ = State::Starting;
  ButtonNavigator buttonNavigator_;
  int selectedIndex_ = 0;
  std::string status_;
  std::string error_;
  unsigned long scanStartedMs_ = 0;
  uint8_t lastCount_ = 0;

  int itemCount() const;
  void startScan();
  void connectSelected();
  std::string itemLabel(int index) const;
};
