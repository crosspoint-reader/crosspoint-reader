#pragma once
#include <Epub.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "MappedInputManager.h"

class EpubReaderMenuActivity final : public ActivityWithSubactivity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction { SELECT_CHAPTER, GO_TO_PERCENT, GO_HOME, DELETE_CACHE };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const std::function<void()>& onBack, const std::function<void(MenuAction)>& onAction)
      : ActivityWithSubactivity("EpubReaderMenu", renderer, mappedInput),
        title(title),
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

  // Fixed menu layout (order matters for up/down navigation).
  const std::vector<MenuItem> menuItems = {{MenuAction::SELECT_CHAPTER, "Go to Chapter"},
                                           {MenuAction::GO_TO_PERCENT, "Go to %"},
                                           {MenuAction::GO_HOME, "Go Home"},
                                           {MenuAction::DELETE_CACHE, "Delete Book Cache"}};

  int selectedIndex = 0;
  bool updateRequired = false;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  std::string title = "Reader Menu";

  const std::function<void()> onBack;
  const std::function<void(MenuAction)> onAction;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
};
