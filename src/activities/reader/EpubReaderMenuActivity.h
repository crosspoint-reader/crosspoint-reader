#pragma once
#include <Epub.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <functional>
#include <vector>
#include <string>

#include "MappedInputManager.h"
#include "../ActivityWithSubactivity.h"

class EpubReaderMenuActivity final : public ActivityWithSubactivity {
 public:
  enum class MenuAction { SELECT_CHAPTER, GO_HOME, DELETE_CACHE };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  const std::function<void()>& onBack,
                                  const std::function<void(MenuAction)>& onAction)
      : ActivityWithSubactivity("EpubReaderMenu", renderer, mappedInput),
        onBack(onBack),
        onAction(onAction) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  struct MenuItem {
    MenuAction action;
    std::string label;
  };

  const std::vector<MenuItem> menuItems = {
      {MenuAction::SELECT_CHAPTER, "Select Chapter"},
      {MenuAction::GO_HOME, "Go Home"},
      {MenuAction::DELETE_CACHE, "Delete Book Cache"}};

  int selectedIndex = 0;
  bool updateRequired = false;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  const std::function<void()> onBack;
  const std::function<void(MenuAction)> onAction;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
};
