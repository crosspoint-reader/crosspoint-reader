#pragma once
#include <Epub.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"

class EpubReaderMenuActivity final : public ActivityWithSubactivity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction { SELECT_CHAPTER, GO_TO_PERCENT, ROTATE_SCREEN, GO_HOME, SYNC, DELETE_CACHE };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const std::function<void(uint8_t)>& onBack,
                                  const std::function<void(MenuAction)>& onAction)
      : ActivityWithSubactivity("EpubReaderMenu", renderer, mappedInput),
        title(title),
        pendingOrientation(currentOrientation),
        currentPage(currentPage),
        totalPages(totalPages),
        bookProgressPercent(bookProgressPercent),
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
  const std::vector<MenuItem> menuItems = {
      {MenuAction::SELECT_CHAPTER, "Go to Chapter"}, {MenuAction::ROTATE_SCREEN, "Reading Orientation"},
      {MenuAction::GO_TO_PERCENT, "Go to %"},        {MenuAction::GO_HOME, "Go Home"},
      {MenuAction::SYNC, "Sync Progress"},           {MenuAction::DELETE_CACHE, "Delete Book Cache"}};

  int selectedIndex = 0;
  bool updateRequired = false;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
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
