#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "activities/ActivityWithSubactivity.h"

class CrossPointSettings;

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE };

// Structure to hold setting information
struct SettingInfo {
  const char* name;                        // Display name of the setting
  SettingType type;                        // Type of setting
  uint8_t CrossPointSettings::* valuePtr;  // Pointer to member in CrossPointSettings (for TOGGLE/ENUM/VALUE)
  std::vector<std::string> enumValues;

  // Bounds for VALUE type settings
  int8_t minValue;
  uint8_t maxValue;

  // Static constructors
  static SettingInfo Toggle(const char* name, uint8_t CrossPointSettings::* ptr) {
    return {name, SettingType::TOGGLE, ptr, {}, 0, 0};
  }

  static SettingInfo Enum(const char* name, uint8_t CrossPointSettings::* ptr, std::vector<std::string> values) {
    return {name, SettingType::ENUM, ptr, std::move(values), 0, 0};
  }

  static SettingInfo Action(const char* name) { return {name, SettingType::ACTION, nullptr, {}, 0, 0}; }

  static SettingInfo Value(const char* name, uint8_t CrossPointSettings::* ptr, int8_t minValue, uint8_t maxValue) {
    return {name, SettingType::VALUE, ptr, {}, minValue, maxValue};
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
