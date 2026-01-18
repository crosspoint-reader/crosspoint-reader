#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "activities/ActivityWithSubactivity.h"

/**
 * Activity for selecting a custom font from /.crosspoint/fonts folder.
 * Lists .epdfont files and allows the user to select one.
 */
class FontSelectionActivity final : public ActivityWithSubactivity {
 public:
  explicit FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const std::function<void()>& onBack)
      : ActivityWithSubactivity("FontSelection", renderer, mappedInput), onBack(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;

  int selectedIndex = 0;
  std::vector<std::string> fontFiles;  // List of font file paths
  std::vector<std::string> fontNames;  // Display names (without path and extension)
  const std::function<void()> onBack;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();
  void loadFontList();
  void handleSelection();

  static constexpr const char* FONTS_DIR = "/.crosspoint/fonts";
};
