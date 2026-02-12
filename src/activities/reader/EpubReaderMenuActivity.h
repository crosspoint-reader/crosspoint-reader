#pragma once
#include <Epub.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

class EpubReaderMenuActivity final : public ActivityWithSubactivity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    SELECT_CHAPTER,
    GO_TO_PERCENT,
    ROTATE_SCREEN,
    LOOKUP,
    LOOKED_UP_WORDS,
    GO_HOME,
    SYNC,
    DELETE_CACHE
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasDictionary,
                                  const std::function<void(uint8_t)>& onBack,
                                  const std::function<void(MenuAction)>& onAction)
      : ActivityWithSubactivity("EpubReaderMenu", renderer, mappedInput),
        title(title),
        pendingOrientation(currentOrientation),
        currentPage(currentPage),
        totalPages(totalPages),
        bookProgressPercent(bookProgressPercent),
        onBack(onBack),
        onAction(onAction) {
    menuItems = {{MenuAction::SELECT_CHAPTER, "Go to Chapter"},
                 {MenuAction::ROTATE_SCREEN, "Reading Orientation"},
                 {MenuAction::GO_TO_PERCENT, "Go to %"}};
    if (hasDictionary) {
      menuItems.push_back({MenuAction::LOOKUP, "Lookup"});
      menuItems.push_back({MenuAction::LOOKED_UP_WORDS, "Lookup History"});
    }
    menuItems.push_back({MenuAction::GO_HOME, "Go Home"});
    menuItems.push_back({MenuAction::SYNC, "Sync Progress"});
    menuItems.push_back({MenuAction::DELETE_CACHE, "Delete Book Cache"});
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  struct MenuItem {
    MenuAction action;
    std::string label;
  };

  std::vector<MenuItem> menuItems;

  int selectedIndex = 0;
  bool updateRequired = false;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  ButtonNavigator buttonNavigator;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  const std::vector<const char*> orientationLabels = {"Portrait", "Landscape CW", "Inverted", "Landscape CCW"};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;

  const std::function<void(uint8_t)> onBack;
  const std::function<void(MenuAction)> onAction;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
};
