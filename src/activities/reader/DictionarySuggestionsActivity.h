#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

class DictionarySuggestionsActivity final : public ActivityWithSubactivity {
 public:
  explicit DictionarySuggestionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         const std::string& originalWord, const std::vector<std::string>& suggestions,
                                         int readerFontId, const std::string& cachePath,
                                         const std::function<void()>& onBack, const std::function<void()>& onDone)
      : ActivityWithSubactivity("DictionarySuggestions", renderer, mappedInput),
        originalWord(originalWord),
        suggestions(suggestions),
        readerFontId(readerFontId),
        cachePath(cachePath),
        onBack(onBack),
        onDone(onDone) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  std::string originalWord;
  std::vector<std::string> suggestions;
  int readerFontId;
  std::string cachePath;
  const std::function<void()> onBack;
  const std::function<void()> onDone;

  int selectedIndex = 0;
  bool updateRequired = false;
  bool pendingBackFromDef = false;
  bool pendingExitToReader = false;
  ButtonNavigator buttonNavigator;

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  void renderScreen();
  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
};
