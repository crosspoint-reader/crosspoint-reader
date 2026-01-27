#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "ThemeContext.h"
#include "ThemeManager.h"
#include "activities/ActivityWithSubactivity.h"

class CrossPointSettings;
struct SettingInfo;

class SettingsActivity final : public ActivityWithSubactivity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  bool subActivityExitPending = false;
  int selectedCategoryIndex = 0;
  int selectedSettingIndex = 0;
  const std::function<void()> onGoHome;

  static constexpr int categoryCount = 4;
  static const char* categoryNames[categoryCount];

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void enterCategoryLegacy(int categoryIndex);

  // Theme support
  ThemeEngine::ThemeContext themeContext;
  void updateThemeContext(bool fullUpdate = false);
  void handleThemeInput();
  void toggleCurrentSetting();

 public:
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                            const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("Settings", renderer, mappedInput), onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
