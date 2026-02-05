#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/ActivityWithSubactivity.h"

// Action items for the System category
struct ActionItem {
  const char* name;
  enum class Type { KOREADER_SYNC, CALIBRE_SETTINGS, CLEAR_CACHE, CHECK_UPDATES };
  Type type;
};

class CategorySettingsActivity final : public ActivityWithSubactivity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  int selectedSettingIndex = 0;
  const char* categoryName;
  const std::vector<const SettingDescriptor*> descriptors;
  const std::vector<ActionItem> actionItems;
  const std::function<void()> onGoBack;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void toggleCurrentSetting();

 public:
  CategorySettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* categoryName,
                           const std::vector<const SettingDescriptor*>& descriptors,
                           const std::vector<ActionItem>& actionItems, const std::function<void()>& onGoBack)
      : ActivityWithSubactivity("CategorySettings", renderer, mappedInput),
        categoryName(categoryName),
        descriptors(descriptors),
        actionItems(actionItems),
        onGoBack(onGoBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
