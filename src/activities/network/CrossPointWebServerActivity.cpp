#include "CrossPointWebServerActivity.h"

#include <GfxRenderer.h>
#include <WiFi.h>

#include "config.h"

void CrossPointWebServerActivity::taskTrampoline(void* param) {
  auto* self = static_cast<CrossPointWebServerActivity*>(param);
  self->displayTaskLoop();
}

void CrossPointWebServerActivity::onEnter() {
  Serial.printf("[%lu] [WEBACT] ========== CrossPointWebServerActivity onEnter ==========\n", millis());
  Serial.printf("[%lu] [WEBACT] [MEM] Free heap at onEnter: %d bytes\n", millis(), ESP.getFreeHeap());

  renderingMutex = xSemaphoreCreateMutex();

  // Reset state
  state = WebServerActivityState::WIFI_SELECTION;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  updateRequired = true;

  xTaskCreate(&CrossPointWebServerActivity::taskTrampoline, "WebServerActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );

  // Turn on WiFi immediately
  Serial.printf("[%lu] [WEBACT] Turning on WiFi...\n", millis());
  WiFi.mode(WIFI_STA);

  // Launch WiFi selection subactivity
  Serial.printf("[%lu] [WEBACT] Launching WifiSelectionActivity...\n", millis());
  wifiSelection.reset(new WifiSelectionActivity(renderer, inputManager,
                                                [this](bool connected) { onWifiSelectionComplete(connected); }));
  wifiSelection->onEnter();
}

void CrossPointWebServerActivity::onExit() {
  Serial.printf("[%lu] [WEBACT] ========== CrossPointWebServerActivity onExit START ==========\n", millis());
  Serial.printf("[%lu] [WEBACT] [MEM] Free heap at onExit start: %d bytes\n", millis(), ESP.getFreeHeap());

  state = WebServerActivityState::SHUTTING_DOWN;

  // Stop the web server first (before disconnecting WiFi)
  stopWebServer();

  // Exit WiFi selection subactivity if still active
  if (wifiSelection) {
    Serial.printf("[%lu] [WEBACT] Exiting WifiSelectionActivity...\n", millis());
    wifiSelection->onExit();
    wifiSelection.reset();
    Serial.printf("[%lu] [WEBACT] WifiSelectionActivity exited\n", millis());
  }

  // CRITICAL: Wait for LWIP stack to flush any pending packets
  Serial.printf("[%lu] [WEBACT] Waiting 500ms for network stack to flush pending packets...\n", millis());
  delay(500);

  // Disconnect WiFi gracefully
  Serial.printf("[%lu] [WEBACT] Disconnecting WiFi (graceful)...\n", millis());
  WiFi.disconnect(false);  // false = don't erase credentials, send disconnect frame
  delay(100);              // Allow disconnect frame to be sent

  Serial.printf("[%lu] [WEBACT] Setting WiFi mode OFF...\n", millis());
  WiFi.mode(WIFI_OFF);
  delay(100);  // Allow WiFi hardware to fully power down

  Serial.printf("[%lu] [WEBACT] [MEM] Free heap after WiFi disconnect: %d bytes\n", millis(), ESP.getFreeHeap());

  // Acquire mutex before deleting task
  Serial.printf("[%lu] [WEBACT] Acquiring rendering mutex before task deletion...\n", millis());
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  // Delete the display task
  Serial.printf("[%lu] [WEBACT] Deleting display task...\n", millis());
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
    Serial.printf("[%lu] [WEBACT] Display task deleted\n", millis());
  }

  // Delete the mutex
  Serial.printf("[%lu] [WEBACT] Deleting mutex...\n", millis());
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  Serial.printf("[%lu] [WEBACT] Mutex deleted\n", millis());

  Serial.printf("[%lu] [WEBACT] [MEM] Free heap at onExit end: %d bytes\n", millis(), ESP.getFreeHeap());
  Serial.printf("[%lu] [WEBACT] ========== CrossPointWebServerActivity onExit COMPLETE ==========\n", millis());
}

void CrossPointWebServerActivity::onWifiSelectionComplete(bool connected) {
  Serial.printf("[%lu] [WEBACT] WifiSelectionActivity completed, connected=%d\n", millis(), connected);

  if (connected) {
    // Get connection info before exiting subactivity
    connectedIP = wifiSelection->getConnectedIP();
    connectedSSID = WiFi.SSID().c_str();

    // Exit the wifi selection subactivity
    wifiSelection->onExit();
    wifiSelection.reset();

    // Start the web server
    startWebServer();
  } else {
    // User cancelled - go back
    onGoBack();
  }
}

void CrossPointWebServerActivity::startWebServer() {
  Serial.printf("[%lu] [WEBACT] Starting web server...\n", millis());

  // Create the web server instance
  webServer.reset(new CrossPointWebServer());
  webServer->begin();

  if (webServer->isRunning()) {
    state = WebServerActivityState::SERVER_RUNNING;
    Serial.printf("[%lu] [WEBACT] Web server started successfully\n", millis());

    // Force an immediate render since we're transitioning from a subactivity
    // that had its own rendering task. We need to make sure our display is shown.
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    render();
    xSemaphoreGive(renderingMutex);
    Serial.printf("[%lu] [WEBACT] Rendered File Transfer screen\n", millis());
  } else {
    Serial.printf("[%lu] [WEBACT] ERROR: Failed to start web server!\n", millis());
    webServer.reset();
    // Go back on error
    onGoBack();
  }
}

void CrossPointWebServerActivity::stopWebServer() {
  if (webServer && webServer->isRunning()) {
    Serial.printf("[%lu] [WEBACT] Stopping web server...\n", millis());
    webServer->stop();
    Serial.printf("[%lu] [WEBACT] Web server stopped\n", millis());
  }
  webServer.reset();
}

void CrossPointWebServerActivity::loop() {
  // Handle different states
  switch (state) {
    case WebServerActivityState::WIFI_SELECTION:
      // Forward loop to WiFi selection subactivity
      if (wifiSelection) {
        wifiSelection->loop();
      }
      break;

    case WebServerActivityState::SERVER_RUNNING:
      // Handle web server requests - call handleClient multiple times per loop
      // to improve responsiveness and upload throughput
      if (webServer && webServer->isRunning()) {
        unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;

        // Log if there's a significant gap between handleClient calls (>100ms)
        if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
          Serial.printf("[%lu] [WEBACT] WARNING: %lu ms gap since last handleClient\n", millis(),
                        timeSinceLastHandleClient);
        }

        // Call handleClient multiple times to process pending requests faster
        // This is critical for upload performance - HTTP file uploads send data
        // in chunks and each handleClient() call processes incoming data
        constexpr int HANDLE_CLIENT_ITERATIONS = 10;
        for (int i = 0; i < HANDLE_CLIENT_ITERATIONS && webServer->isRunning(); i++) {
          webServer->handleClient();
        }
        lastHandleClientTime = millis();
      }

      // Handle exit on Back button
      if (inputManager.wasPressed(InputManager::BTN_BACK)) {
        onGoBack();
        return;
      }
      break;

    case WebServerActivityState::SHUTTING_DOWN:
      // Do nothing - waiting for cleanup
      break;
  }
}

void CrossPointWebServerActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void CrossPointWebServerActivity::render() const {
  // Only render our own UI when server is running
  // WiFi selection handles its own rendering
  if (state == WebServerActivityState::SERVER_RUNNING) {
    renderer.clearScreen();
    renderServerRunning();
    renderer.displayBuffer();
  }
}

void CrossPointWebServerActivity::renderServerRunning() const {
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_FONT_ID);
  const auto top = (pageHeight - height * 5) / 2;

  renderer.drawCenteredText(READER_FONT_ID, top - 30, "File Transfer", true, BOLD);

  std::string ssidInfo = "Network: " + connectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  renderer.drawCenteredText(UI_FONT_ID, top + 10, ssidInfo.c_str(), true, REGULAR);

  std::string ipInfo = "IP Address: " + connectedIP;
  renderer.drawCenteredText(UI_FONT_ID, top + 40, ipInfo.c_str(), true, REGULAR);

  // Show web server URL prominently
  std::string webInfo = "http://" + connectedIP + "/";
  renderer.drawCenteredText(UI_FONT_ID, top + 70, webInfo.c_str(), true, BOLD);

  renderer.drawCenteredText(SMALL_FONT_ID, top + 110, "Open this URL in your browser", true, REGULAR);

  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, "Press BACK to exit", true, REGULAR);
}
