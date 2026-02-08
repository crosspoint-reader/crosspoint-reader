#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include <functional>

#include "../ActivityWithSubactivity.h"
#include "components/UITheme.h"

class MappedInputManager;

/**
 * Activity for selecting UI language
 */
class LanguageSelectActivity final : public ActivityWithSubactivity {
 public:
  explicit LanguageSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  const std::function<void()>& onBack)
      : ActivityWithSubactivity("LanguageSelect", renderer, mappedInput), onBack(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  static void taskTrampoline(void* param);
  void displayTaskLoop();
  void render();
  void handleSelection();

  std::function<void()> onBack;
  int selectedIndex = 0;
  static constexpr int totalItems = 5;  // English, Spanish, Italian, Swedish, French

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  volatile bool updateRequired = false;
};
