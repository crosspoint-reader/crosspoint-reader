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
                                 int readerFontId, const std::function<void()>& onBack,
                                 const std::function<void()>& onDone)
      : ActivityWithSubactivity("LookedUpWords", renderer, mappedInput),
        cachePath(cachePath),
        readerFontId(readerFontId),
        onBack(onBack),
        onDone(onDone) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  std::string cachePath;
  int readerFontId;
  const std::function<void()> onBack;
  const std::function<void()> onDone;

  std::vector<std::string> words;
  int selectedIndex = 0;
  bool updateRequired = false;
  bool pendingBackFromDef = false;
  bool pendingExitToReader = false;
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
