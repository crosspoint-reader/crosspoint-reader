#pragma once
#include <cstdint>
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EmergencyActivity final : public Activity {
 public:
  explicit EmergencyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Emergency", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == ARMED || state == CHECK_IN || state == TRIGGERED; }
  bool skipLoopDelay() override { return state == TRIGGERED; }

 private:
  enum State { CONFIG, ARMED, CHECK_IN, TRIGGERED };
  enum Mode { PANIC_MANUAL, DEAD_MAN_SWITCH };

  State state = CONFIG;
  Mode mode = PANIC_MANUAL;
  ButtonNavigator buttonNavigator;

  // Config
  int configIndex = 0;
  static constexpr int CONFIG_ITEMS = 6;
  // intervals in ms: 1min, 5min, 10min, 30min
  static constexpr unsigned long INTERVALS[] = {60000, 300000, 600000, 1800000};
  static constexpr int INTERVAL_COUNT = 4;
  int intervalIndex = 1;  // default 5 minutes

  bool broadcastWifi = true;
  bool broadcastMesh = true;
  bool showMedical = true;

  // Dead man's switch
  unsigned long lastCheckIn = 0;
  int missedCheckIns = 0;
  static constexpr int MAX_MISSED = 2;

  // Triggered state
  bool sosActive = false;
  unsigned long triggerTime = 0;

  // Medical info cache
  struct MedInfo {
    char name[32];
    char bloodType[8];
    char allergies[128];
    char emergencyContact[64];
    char emergencyPhone[20];
  };
  MedInfo medInfo = {};
  bool medInfoLoaded = false;

  // Confirm counter for panic trigger
  int confirmPressCount = 0;
  unsigned long lastConfirmTime = 0;

  // Mesh SOS periodic broadcast
  bool espnowInitialized = false;
  unsigned long lastMeshSos = 0;
  static constexpr unsigned long MESH_SOS_INTERVAL_MS = 5000;

  void triggerEmergency();
  void stopEmergency();
  void sendMeshSos();
  void loadMedicalInfo();

  void renderConfig() const;
  void renderArmed() const;
  void renderCheckIn() const;
  void renderTriggered() const;
};
