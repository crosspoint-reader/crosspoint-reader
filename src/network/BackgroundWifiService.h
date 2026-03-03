#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>

/**
 * BackgroundWifiService manages a silent WiFi connection and web server
 * that runs alongside the normal activity stack. Used for WiFi auto-connect
 * on wake from sleep so users can push files or sync progress without
 * manually opening File Transfer.
 *
 * Lifecycle:
 *   start(ssid, password) — spawns a FreeRTOS task that connects + serves
 *   stop()                — tears down server, disconnects WiFi, kills task
 *   isRunning()           — true while task is active
 *   getRequestCount()     — HTTP requests handled since start()
 */
class CrossPointWebServer;

class BackgroundWifiService {
  static BackgroundWifiService instance;

  CrossPointWebServer* server = nullptr;
  TaskHandle_t taskHandle = nullptr;
  volatile bool stopRequested = false;
  volatile bool keepWifiOnStop = false;
  volatile bool connected = false;
  volatile bool wifiOwned = false;
  volatile uint32_t requestCount = 0;

  // FreeRTOS task entry point
  static void taskEntry(void* arg);
  void run(const char* ssid, const char* password, bool useCurrentConnection);

  // Stack size: 4096 bytes — WiFi connect + WebServer + handler parsing
  static constexpr uint32_t TASK_STACK = 4096;
  static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;

 public:
  BackgroundWifiService() = default;
  BackgroundWifiService(const BackgroundWifiService&) = delete;
  BackgroundWifiService& operator=(const BackgroundWifiService&) = delete;

  static BackgroundWifiService& getInstance() { return instance; }

  // Start background WiFi + web server (no-op if already running)
  void start(const char* ssid, const char* password);

  // Start the background web server using the current STA connection.
  void startUsingCurrentConnection();

  // Stop the background task. If keepWifi is true, preserve the current STA
  // connection so a foreground activity can reuse it.
  void stop(bool keepWifi = false);

  bool isRunning() const { return taskHandle != nullptr; }
  bool isConnected() const { return connected; }
  uint32_t getRequestCount() const { return requestCount; }
  bool hadApiActivity() const { return requestCount > 0; }
};

#define BG_WIFI BackgroundWifiService::getInstance()
