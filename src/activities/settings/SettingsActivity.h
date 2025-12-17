#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../Activity.h"

class CrossPointSettings;

// Structure to hold setting information
struct SettingInfo {
  const char* name;                        // Display name of the setting
  uint8_t CrossPointSettings::* valuePtr;  // Pointer to member in CrossPointSettings
};

class SettingsActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  int selectedSettingIndex = 0;  // Currently selected setting
  const std::function<void()> onGoHome;

  // Static settings list
  static constexpr int settingsCount = 2;  // Number of settings
  static const SettingInfo settingsList[settingsCount];

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void toggleCurrentSetting();

 public:
  explicit SettingsActivity(GfxRenderer& renderer, InputManager& inputManager, const std::function<void()>& onGoHome)
      : Activity(renderer, inputManager), onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
