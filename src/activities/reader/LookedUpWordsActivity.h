#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

class LookedUpWordsActivity final : public ActivityWithSubactivity {
 public:
  explicit LookedUpWordsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& cachePath,
                                 const std::function<void()>& onBack,
                                 const std::function<void(const std::string&)>& onSelectWord)
      : ActivityWithSubactivity("LookedUpWords", renderer, mappedInput),
        cachePath(cachePath),
        onBack(onBack),
        onSelectWord(onSelectWord) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  std::string cachePath;
  const std::function<void()> onBack;
  const std::function<void(const std::string&)> onSelectWord;

  std::vector<std::string> words;
  int selectedIndex = 0;
  bool updateRequired = false;
  ButtonNavigator buttonNavigator;

  // Delete confirmation state
  bool deleteConfirmMode = false;
  bool ignoreNextConfirmRelease = false;
  int pendingDeleteIndex = 0;

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  int getPageItems() const;
  void renderScreen();
  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
};
