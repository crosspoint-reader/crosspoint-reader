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
  const char* name;                           // Display name of the setting
  SettingType type;                           // Type of setting
  uint8_t CrossPointSettings::* valuePtr;     // Pointer for 8-bit settings (TOGGLE/ENUM)
  uint16_t CrossPointSettings::* valuePtr16;  // Pointer for 16-bit VALUE settings
  std::vector<std::string> enumValues;

  struct ValueRange {
    uint16_t min;
    uint16_t max;
    uint16_t step;
  };
  // Bounds/step for VALUE type settings
  ValueRange valueRange;

  // Static constructors
  static SettingInfo Toggle(const char* name, uint8_t CrossPointSettings::* ptr) {
    return {name, SettingType::TOGGLE, ptr, nullptr};
  }

  static SettingInfo Enum(const char* name, uint8_t CrossPointSettings::* ptr, std::vector<std::string> values) {
    return {name, SettingType::ENUM, ptr, nullptr, std::move(values)};
  }

  static SettingInfo Action(const char* name) { return {name, SettingType::ACTION, nullptr, nullptr}; }

  static SettingInfo Value(const char* name, uint16_t CrossPointSettings::* ptr, const ValueRange valueRange) {
    return {name, SettingType::VALUE, nullptr, ptr, {}, valueRange};
  }

  static SettingInfo Value(const char* name, uint8_t CrossPointSettings::* ptr, const ValueRange valueRange) {
    return {name, SettingType::VALUE, ptr, nullptr, {}, valueRange};
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
