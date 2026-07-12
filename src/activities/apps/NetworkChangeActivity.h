#pragma once
#include <cstdint>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class NetworkChangeActivity final : public Activity {
 public:
  explicit NetworkChangeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("NetworkChange", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { MENU, TAKING_SNAPSHOT, COMPARING, RESULTS };

  struct Device {
    char name[33];  // SSID or BLE name
    char mac[18];
    int8_t rssi;
    uint8_t type;  // 0 = wifi, 1 = ble
  };

  struct Snapshot {
    std::vector<Device> devices;
    unsigned long timestamp;
    char label[32];
  };

  State state = MENU;
  int menuIndex = 0;
  ButtonNavigator buttonNavigator;
  int spinnerFrame = 0;
  unsigned long lastSpinnerUpdate = 0;

  Snapshot current;
  Snapshot saved;

  std::vector<Device> newDevices;
  std::vector<Device> goneDevices;
  int resultIndex = 0;

  static constexpr const char* SNAPSHOT_DIR = "/biscuit/snapshots";
  static constexpr int MAX_DEVICES = 64;

  void takeSnapshot();
  bool loadSnapshot();
  void compareSnapshots();
  void saveSnapshotToFile(const Snapshot& snap);

  static constexpr const char* MENU_ITEMS[] = {
      "Take Snapshot",
      "Compare with Saved",
      "Back"};
  static constexpr int MENU_COUNT = 3;
};
