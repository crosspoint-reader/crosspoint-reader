#include "BackgroundWifiService.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "network/CrossPointWebServer.h"

BackgroundWifiService BackgroundWifiService::instance;

// Structure passed to the FreeRTOS task so it owns copies of the credentials
// and we don't hold pointers into the caller's stack after start() returns.
struct WifiTaskParams {
  char ssid[64];
  char password[64];
  bool useCurrentConnection = false;
};

void BackgroundWifiService::taskEntry(void* arg) {
  auto* params = static_cast<WifiTaskParams*>(arg);
  instance.run(params->ssid, params->password, params->useCurrentConnection);
  delete params;
}

void BackgroundWifiService::run(const char* ssid, const char* password, const bool useCurrentConnection) {
  wifiOwned = false;

  if (useCurrentConnection) {
    LOG_DBG("BGWIFI", "Starting background web server on existing WiFi connection");
    if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
      LOG_DBG("BGWIFI", "Existing WiFi connection unavailable");
      goto cleanup;
    }
  } else {
    LOG_DBG("BGWIFI", "Starting background WiFi, SSID: %s", ssid);

    // ── Connect ────────────────────────────────────────────────────────────
    wifiOwned = true;
    WiFi.mode(WIFI_STA);
    if (password && password[0] != '\0') {
      WiFi.begin(ssid, password);
    } else {
      WiFi.begin(ssid);
    }

    const unsigned long connectDeadline = millis() + CONNECT_TIMEOUT_MS;
    while (WiFi.status() != WL_CONNECTED && millis() < connectDeadline) {
      if (stopRequested) {
        LOG_DBG("BGWIFI", "Stop requested during connect");
        goto cleanup;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
      LOG_DBG("BGWIFI", "Connection timed out");
      goto cleanup;
    }
  }

  {
    const IPAddress ip = WiFi.localIP();
    LOG_DBG("BGWIFI", "Connected! IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    connected = true;

    // ── Start web server ──────────────────────────────────────────────────
    server = new CrossPointWebServer();
    server->begin();

    if (!server->isRunning()) {
      LOG_ERR("BGWIFI", "Web server failed to start");
      delete server;
      server = nullptr;
      goto cleanup;
    }

    LOG_DBG("BGWIFI", "Background web server running on port %d", server->getPort());

    // ── Service loop ──────────────────────────────────────────────────────
    while (!stopRequested) {
      esp_task_wdt_reset();

      if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
        LOG_DBG("BGWIFI", "WiFi disconnected; stopping background server");
        break;
      }

      server->handleClient();
      requestCount = server->getRequestCount();  // Propagate to volatile field
      vTaskDelay(pdMS_TO_TICKS(1));              // Yield to scheduler
    }

    LOG_DBG("BGWIFI", "Background task stopping. Requests served: %lu", requestCount);

    server->stop();
    delete server;
    server = nullptr;
  }

cleanup:
  if (wifiOwned && !(stopRequested && keepWifiOnStop)) {
    WiFi.disconnect(false);
    delay(30);
    WiFi.mode(WIFI_OFF);
    delay(30);
  }

  connected = false;
  wifiOwned = false;
  keepWifiOnStop = false;

  // Signal stop() that we've exited, then self-delete
  taskHandle = nullptr;
  vTaskDelete(nullptr);
}

void BackgroundWifiService::start(const char* ssid, const char* password) {
  if (taskHandle != nullptr) {
    LOG_DBG("BGWIFI", "Already running, ignoring start()");
    return;
  }

  stopRequested = false;
  keepWifiOnStop = false;
  connected = false;
  wifiOwned = false;
  requestCount = 0;

  // Heap-allocate params so the pointers remain valid after this function returns
  auto* params = new WifiTaskParams();
  strncpy(params->ssid, ssid, sizeof(params->ssid) - 1);
  params->ssid[sizeof(params->ssid) - 1] = '\0';
  strncpy(params->password, password ? password : "", sizeof(params->password) - 1);
  params->password[sizeof(params->password) - 1] = '\0';
  params->useCurrentConnection = false;

  const BaseType_t result =
      xTaskCreate(&BackgroundWifiService::taskEntry, "bgwifi", TASK_STACK, params, 1, &taskHandle);

  if (result != pdPASS) {
    LOG_ERR("BGWIFI", "Failed to create task (heap: %d bytes free)", ESP.getFreeHeap());
    delete params;
    taskHandle = nullptr;
  } else {
    LOG_DBG("BGWIFI", "Background WiFi task started");
  }
}

void BackgroundWifiService::startUsingCurrentConnection() {
  if (taskHandle != nullptr) {
    LOG_DBG("BGWIFI", "Already running, ignoring startUsingCurrentConnection()");
    return;
  }

  stopRequested = false;
  keepWifiOnStop = false;
  connected = false;
  wifiOwned = false;
  requestCount = 0;

  auto* params = new WifiTaskParams();
  params->ssid[0] = '\0';
  params->password[0] = '\0';
  params->useCurrentConnection = true;

  const BaseType_t result =
      xTaskCreate(&BackgroundWifiService::taskEntry, "bgwifi", TASK_STACK, params, 1, &taskHandle);

  if (result != pdPASS) {
    LOG_ERR("BGWIFI", "Failed to create task (heap: %d bytes free)", ESP.getFreeHeap());
    delete params;
    taskHandle = nullptr;
  } else {
    LOG_DBG("BGWIFI", "Background WiFi task started on existing connection");
  }
}

void BackgroundWifiService::stop(const bool keepWifi) {
  if (taskHandle == nullptr) {
    return;
  }

  LOG_DBG("BGWIFI", "Requesting stop...");
  keepWifiOnStop = keepWifi;
  stopRequested = true;

  // Wait for the task to exit (max 3 seconds)
  constexpr unsigned long STOP_TIMEOUT_MS = 3000;
  const unsigned long deadline = millis() + STOP_TIMEOUT_MS;
  while (taskHandle != nullptr && millis() < deadline) {
    delay(10);
  }

  if (taskHandle != nullptr) {
    // Task didn't exit cleanly — force-delete as last resort
    LOG_ERR("BGWIFI", "Task did not exit within timeout, force-deleting");
    vTaskDelete(taskHandle);
    taskHandle = nullptr;
    connected = false;
    if (wifiOwned && !keepWifi) {
      WiFi.disconnect(false);
      WiFi.mode(WIFI_OFF);
    }
    wifiOwned = false;
    keepWifiOnStop = false;
  }

  LOG_DBG("BGWIFI", "Stopped. Total requests served: %lu", requestCount);
}
