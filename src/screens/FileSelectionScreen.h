#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "Screen.h"

class FileSelectionScreen final : public Screen {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  std::string basepath = "/";
  std::vector<std::string> files;
  int selectorIndex = 0;
  bool updateRequired = false;
  const std::function<void(const std::string&)> onSelect;
  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void loadFiles();

 public:
  explicit FileSelectionScreen(GfxRenderer& renderer, InputManager& inputManager,
                               const std::function<void(const std::string&)>& onSelect,
                               const std::function<void()>& onGoHome)
      : Screen(renderer, inputManager), onSelect(onSelect), onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void handleInput() override;
};
