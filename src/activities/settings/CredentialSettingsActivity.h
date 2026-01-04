#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "activities/ActivityWithSubactivity.h"

/**
 * CredentialSettingsActivity allows users to configure credentials for:
 * - FTP server (username and password)
 * - HTTP server (username and password)
 * - WiFi Hotspot (SSID and password)
 */
class CredentialSettingsActivity final : public ActivityWithSubactivity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  int selectedIndex = 0;  // Currently selected credential field
  const std::function<void()> onGoBack;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void selectCurrentField();

 public:
  explicit CredentialSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                      const std::function<void()>& onGoBack)
      : ActivityWithSubactivity("CredentialSettings", renderer, mappedInput), onGoBack(onGoBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
