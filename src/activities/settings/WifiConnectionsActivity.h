#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>

#include "activities/ActivityWithSubactivity.h"

/**
 * Activity for managing saved WiFi connections.
 * Shows a list of saved WiFi networks and allows deletion with confirmation.
 */
class WifiConnectionsActivity final : public ActivityWithSubactivity {
 public:
  explicit WifiConnectionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const std::function<void()>& onBack)
      : ActivityWithSubactivity("WifiConnections", renderer, mappedInput), onBack(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  enum class State {
    LIST,
    SETTINGS_MENU
  };

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  State state = State::LIST;
  size_t selectorIndex = 0;
  int settingsSelection = 0;
  std::string selectedNetwork;
  unsigned long enterTime = 0;
  const std::function<void()> onBack;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();
  void handleSettings();
  void setDefault();
  void removeDefault();
  void deleteNetwork();
  void cancelSettings();
};

