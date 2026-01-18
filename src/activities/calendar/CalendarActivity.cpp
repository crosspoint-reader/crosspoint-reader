#include "CalendarActivity.h"

#include <GfxRenderer.h>
#include <HTTPClient.h>
#include <SDCardManager.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <time.h>

#include "../../CrossPointSettings.h"
#include "../../CrossPointState.h"
#include "../../WifiCredentialStore.h"
#include "../../fontIds.h"

// External functions from main.cpp
extern void exitActivity();
extern void enterNewActivity(Activity* activity);
extern void enterDeepSleep();
extern void enterCalendarDeepSleep(uint8_t refreshHours);

void CalendarActivity::onEnter() {
  Activity::onEnter();
  state = CalendarState::INIT;
  stateStartTime = millis();

  Serial.printf("[%lu] [CAL] Calendar mode starting\n", millis());

  // Show status and begin WiFi connection
  renderStatus("Connecting...");
  startWifiConnection();
}

void CalendarActivity::startWifiConnection() {
  Serial.printf("[%lu] [CAL] Loading WiFi credentials\n", millis());

  // Load saved credentials
  WIFI_STORE.loadFromFile();

  const auto& credentials = WIFI_STORE.getCredentials();
  if (credentials.empty()) {
    handleError("No saved WiFi");
    return;
  }

  // Use first saved network
  const auto& cred = credentials[0];
  Serial.printf("[%lu] [CAL] Connecting to: %s\n", millis(), cred.ssid.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.begin(cred.ssid.c_str(), cred.password.c_str());

  state = CalendarState::CONNECTING_WIFI;
  stateStartTime = millis();
}

bool CalendarActivity::checkWifiConnection() { return WiFi.status() == WL_CONNECTED; }

bool CalendarActivity::fetchAndSaveImage() {
  Serial.printf("[%lu] [CAL] Fetching image from: %s\n", millis(), SETTINGS.calendarServerUrl);

  HTTPClient http;
  http.begin(SETTINGS.calendarServerUrl);
  http.setTimeout(30000);
  http.setConnectTimeout(10000);

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[%lu] [CAL] HTTP error: %d\n", millis(), httpCode);
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  Serial.printf("[%lu] [CAL] Content length: %d bytes\n", millis(), contentLength);

  // Open file for writing using SdMan
  FsFile file;
  if (!SdMan.openFileForWrite("CAL", "/sleep.bmp", file)) {
    Serial.printf("[%lu] [CAL] Failed to open /sleep.bmp for writing\n", millis());
    http.end();
    return false;
  }

  // Stream response to file
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[512];
  int totalWritten = 0;

  while (http.connected() && (contentLength > 0 || contentLength == -1)) {
    size_t available = stream->available();
    if (available) {
      size_t toRead = min(available, sizeof(buffer));
      size_t bytesRead = stream->readBytes(buffer, toRead);

      if (bytesRead > 0) {
        file.write(buffer, bytesRead);
        totalWritten += bytesRead;

        if (contentLength > 0) {
          contentLength -= bytesRead;
        }
      }
    } else {
      delay(10);
    }

    // Check for timeout
    if (millis() - stateStartTime > HTTP_TIMEOUT_MS) {
      Serial.printf("[%lu] [CAL] HTTP timeout during download\n", millis());
      file.close();
      http.end();
      return false;
    }

    // Break if we've received everything
    if (contentLength == 0) {
      break;
    }
  }

  file.close();
  http.end();

  Serial.printf("[%lu] [CAL] Saved %d bytes to /sleep.bmp\n", millis(), totalWritten);
  return totalWritten > 0;
}

void CalendarActivity::handleError(const char* message) {
  Serial.printf("[%lu] [CAL] Error: %s\n", millis(), message);
  errorMessage = message;
  state = CalendarState::ERROR;
  stateStartTime = millis();

  // For now, just log - we'll use default sleep screen on error
}

void CalendarActivity::renderStatus(const char* status) {
  Serial.printf("[%lu] [CAL] Status: %s\n", millis(), status);

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, status, true, EpdFontFamily::BOLD);
  renderer.displayBuffer();
}

void CalendarActivity::loop() {
  switch (state) {
    case CalendarState::INIT:
      // Should not reach here - onEnter handles init
      break;

    case CalendarState::CONNECTING_WIFI:
      if (checkWifiConnection()) {
        Serial.printf("[%lu] [CAL] WiFi connected, IP: %s\n", millis(), WiFi.localIP().toString().c_str());

        // Sync time via NTP
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 5000)) {
          Serial.printf("[%lu] [CAL] NTP time synced\n", millis());
        }

        renderStatus("Fetching...");
        state = CalendarState::FETCHING_IMAGE;
        stateStartTime = millis();
      } else if (millis() - stateStartTime > WIFI_TIMEOUT_MS) {
        handleError("WiFi timeout");
      }
      break;

    case CalendarState::FETCHING_IMAGE:
      if (fetchAndSaveImage()) {
        Serial.printf("[%lu] [CAL] Image saved successfully\n", millis());

        // Save fetch timestamp
        time_t now;
        time(&now);
        if (now > 1700000000) {  // Sanity check - after Nov 2023
          APP_STATE.lastCalendarFetch = now;
          APP_STATE.saveToFile();
          Serial.printf("[%lu] [CAL] Saved fetch timestamp: %lu\n", millis(), (unsigned long)now);
        }

        renderStatus("Image saved!");
        state = CalendarState::RENDERING;
      } else {
        // Check if we have a cached image
        if (SdMan.exists("/sleep.bmp")) {
          Serial.printf("[%lu] [CAL] Fetch failed, using cached image\n", millis());
          state = CalendarState::RENDERING;
        } else {
          handleError("Fetch failed");
        }
      }
      break;

    case CalendarState::RENDERING:
      Serial.printf("[%lu] [CAL] Rendering complete, entering deep sleep\n", millis());
      enterCalendarDeepSleep(SETTINGS.calendarRefreshHours);
      // Never reaches here - device enters deep sleep
      break;

    case CalendarState::ERROR:
      // Wait 3 seconds showing error, then sleep anyway (use cached image if available)
      if (millis() - stateStartTime > 3000) {
        if (SdMan.exists("/sleep.bmp")) {
          state = CalendarState::RENDERING;
        } else {
          // No cached image - just sleep with default screen and try again later
          enterCalendarDeepSleep(SETTINGS.calendarRefreshHours);
        }
      }
      break;
  }
}
