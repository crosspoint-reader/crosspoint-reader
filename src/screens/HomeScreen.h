#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "Screen.h"

class HomeScreen final : public Screen {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int selectorIndex = 0;
  bool updateRequired = false;
  const std::function<void()> onFileSelectionOpen;
  const std::function<void()> onSettingsOpen;

  static constexpr int menuItemCount = 2;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;

 public:
  explicit HomeScreen(GfxRenderer& renderer, InputManager& inputManager,
                      const std::function<void()>& onFileSelectionOpen, const std::function<void()>& onSettingsOpen)
      : Screen(renderer, inputManager), onFileSelectionOpen(onFileSelectionOpen), onSettingsOpen(onSettingsOpen) {}
  void onEnter() override;
  void onExit() override;
  void handleInput() override;
};
