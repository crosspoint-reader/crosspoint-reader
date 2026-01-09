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

  struct ValueRange {
    uint8_t min;
    uint8_t max;
    uint8_t step;
  };
  // Bounds/step for VALUE type settings
  ValueRange valueRange;

  // Static constructors
  static SettingInfo Toggle(const char* name, uint8_t CrossPointSettings::* ptr) {
    return {name, SettingType::TOGGLE, ptr};
  }

  static SettingInfo Enum(const char* name, uint8_t CrossPointSettings::* ptr, std::vector<std::string> values) {
    return {name, SettingType::ENUM, ptr, std::move(values)};
  }

  static SettingInfo Action(const char* name) { return {name, SettingType::ACTION, nullptr}; }

  static SettingInfo Value(const char* name, uint8_t CrossPointSettings::* ptr, const ValueRange valueRange) {
    return {name, SettingType::VALUE, ptr, {}, valueRange};
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
