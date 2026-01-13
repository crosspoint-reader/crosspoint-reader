#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "activities/ActivityWithSubactivity.h"

class CrossPointSettings;

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING };

// Structure to hold setting information
struct SettingInfo {
  const char* key;                         // JSON key for web API (nullptr for ACTION types)
  const char* name;                        // Display name of the setting
  SettingType type;                        // Type of setting
  uint8_t CrossPointSettings::* valuePtr;  // Pointer to member in CrossPointSettings (for TOGGLE/ENUM/VALUE)
  char* stringPtr;                         // Pointer to char array (for STRING type)
  size_t stringMaxLen;                     // Max length for STRING type
  bool hideFromDeviceUI;                   // Hide from device settings menu (but show in web API)
  std::vector<std::string> enumValues;

  struct ValueRange {
    uint8_t min;
    uint8_t max;
    uint8_t step;
  };
  // Bounds/step for VALUE type settings
  ValueRange valueRange;

  // Static constructors
  static SettingInfo Toggle(const char* key, const char* name, uint8_t CrossPointSettings::* ptr) {
    return {key, name, SettingType::TOGGLE, ptr, nullptr, 0, false, {}, {}};
  }

  static SettingInfo Enum(const char* key, const char* name, uint8_t CrossPointSettings::* ptr,
                          std::vector<std::string> values) {
    return {key, name, SettingType::ENUM, ptr, nullptr, 0, false, std::move(values), {}};
  }

  static SettingInfo Action(const char* name) {
    return {nullptr, name, SettingType::ACTION, nullptr, nullptr, 0, false, {}, {}};
  }

  static SettingInfo Value(const char* key, const char* name, uint8_t CrossPointSettings::* ptr,
                           const ValueRange valueRange) {
    return {key, name, SettingType::VALUE, ptr, nullptr, 0, false, {}, valueRange};
  }

  static SettingInfo String(const char* key, const char* name, char* ptr, size_t maxLen, bool hideFromDeviceUI = false) {
    return {key, name, SettingType::STRING, nullptr, ptr, maxLen, hideFromDeviceUI, {}, {}};
  }
};

class SettingsActivity final : public ActivityWithSubactivity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  int selectedSettingIndex = 0;  // Currently selected setting
  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void toggleCurrentSetting();

 public:
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                            const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("Settings", renderer, mappedInput), onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
