#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "activities/ActivityWithSubactivity.h"

class CrossPointSettings;

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE };

struct SettingInfo {
  StrId nameId;
  SettingType type;
  uint8_t CrossPointSettings::* valuePtr;
  std::vector<StrId> enumValues;

  struct ValueRange {
    uint8_t min;
    uint8_t max;
    uint8_t step;
  };
  ValueRange valueRange;

  static SettingInfo Toggle(StrId nameId, uint8_t CrossPointSettings::* ptr) {
    return {nameId, SettingType::TOGGLE, ptr};
  }

  static SettingInfo Enum(StrId nameId, uint8_t CrossPointSettings::* ptr, std::vector<StrId> values) {
    return {nameId, SettingType::ENUM, ptr, std::move(values)};
  }

  static SettingInfo Action(StrId nameId) { return {nameId, SettingType::ACTION, nullptr}; }

  static SettingInfo Value(StrId nameId, uint8_t CrossPointSettings::* ptr, const ValueRange valueRange) {
    return {nameId, SettingType::VALUE, ptr, {}, valueRange};
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
