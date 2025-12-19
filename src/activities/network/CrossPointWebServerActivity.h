#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <memory>
#include <string>

#include "../Activity.h"
#include "WifiSelectionActivity.h"

// Web server activity states
enum class WebServerActivityState {
  WIFI_SELECTION,  // WiFi selection subactivity is active
  SERVER_RUNNING,  // Web server is running and handling requests
  SHUTTING_DOWN    // Shutting down server and WiFi
};

/**
 * CrossPointWebServerActivity is the entry point for file transfer functionality.
 * It:
 * - Immediately turns on WiFi and launches WifiSelectionActivity on enter
 * - When WifiSelectionActivity completes successfully, starts the CrossPointWebServer
 * - Handles client requests in its loop() function
 * - Cleans up the server and shuts down WiFi on exit
 */
class CrossPointWebServerActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  WebServerActivityState state = WebServerActivityState::WIFI_SELECTION;
  const std::function<void()> onGoBack;

  // WiFi selection subactivity
  std::unique_ptr<WifiSelectionActivity> wifiSelection;

  // Server status
  std::string connectedIP;
  std::string connectedSSID;

  // Performance monitoring
  unsigned long lastHandleClientTime = 0;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void renderServerRunning() const;

  void onWifiSelectionComplete(bool connected);
  void startWebServer();
  void stopWebServer();

 public:
  explicit CrossPointWebServerActivity(GfxRenderer& renderer, InputManager& inputManager,
                                       const std::function<void()>& onGoBack)
      : Activity(renderer, inputManager), onGoBack(onGoBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
