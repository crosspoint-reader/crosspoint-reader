#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "../Activity.h"

// Enum for network mode selection
enum class NetworkMode { JOIN_NETWORK, CREATE_HOTSPOT };

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 */
class NetworkModeSelectionActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int selectedIndex = 0;
  bool updateRequired = false;
  const std::function<void(NetworkMode)> onModeSelected;
  const std::function<void()> onCancel;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;

 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, InputManager& inputManager,
                                        const std::function<void(NetworkMode)>& onModeSelected,
                                        const std::function<void()>& onCancel)
      : Activity("NetworkModeSelection", renderer, inputManager), onModeSelected(onModeSelected), onCancel(onCancel) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
