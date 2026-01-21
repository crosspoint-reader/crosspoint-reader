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

struct SettingInfo {
  const char* name;
  SettingType type;
  uint8_t CrossPointSettings::* valuePtr;
  std::vector<std::string> enumValues;

  struct ValueRange {
    uint8_t min;
    uint8_t max;
    uint8_t step;
  };
  ValueRange valueRange;

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

class CategorySettingsActivity final : public ActivityWithSubactivity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  int selectedSettingIndex = 0;
  const char* categoryName;
  const SettingInfo* settingsList;
  int settingsCount;
  const std::function<void()> onGoBack;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void toggleCurrentSetting();

 public:
  CategorySettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* categoryName,
                           const SettingInfo* settingsList, int settingsCount, const std::function<void()>& onGoBack)
      : ActivityWithSubactivity("CategorySettings", renderer, mappedInput),
        categoryName(categoryName),
        settingsList(settingsList),
        settingsCount(settingsCount),
        onGoBack(onGoBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
