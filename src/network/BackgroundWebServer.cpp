#include "BackgroundWebServer.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <HardwareSerial.h>
#include <Logging.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointWebServer.h"
#include "FeatureFlags.h"
#include "Logging.h"
#include "core/features/FeatureModules.h"
#include "util/NetworkNames.h"
#include "util/TimeSync.h"

namespace {

void ntpSyncTask(void* /*param*/) {
  TimeSync::syncTimeWithNtpLowMemory();
  vTaskDelete(nullptr);
}

bool findBestCredential(const std::vector<WifiCredential>& credentials, const int16_t scanCount, std::string& outSsid,
                        std::string& outPassword) {
  int bestRssi = -1000;
  bool found = false;
  for (int i = 0; i < scanCount; i++) {
    const std::string ssid = WiFi.SSID(i).c_str();
    if (ssid.empty()) {
      continue;
    }
    auto it = std::find_if(credentials.begin(), credentials.end(),
                           [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });
    if (it != credentials.end()) {
      const int rssi = WiFi.RSSI(i);
      if (!found || rssi > bestRssi) {
        bestRssi = rssi;
        outSsid = it->ssid;
        outPassword = it->password;
        found = true;
      }
    }
  }
  return found;
}
}  // namespace

BackgroundWebServer& BackgroundWebServer::getInstance() {
  static BackgroundWebServer instance;
  return instance;
}

bool BackgroundWebServer::isRunning() const { return server && server->isRunning(); }

bool BackgroundWebServer::shouldPreventAutoSleep() const {
  if (!usbConnectedCached || !allowRunCached) {
    return false;
  }
  return state == State::SCANNING || state == State::CONNECTING || state == State::RUNNING;
}

bool BackgroundWebServer::wantsFastLoop() const { return isRunning(); }

void BackgroundWebServer::invalidateCredentialsCache() {
  credentialsLoaded = false;
  credentials.clear();
  LOG_INF("BWS", "Credentials cache invalidated");
}

void BackgroundWebServer::ensureCredentialsLoaded() {
  if (credentialsLoaded) {
    return;
  }
  // WIFI_STORE is loaded in setup() before any background display tasks,
  // so we just copy the cached credentials without SD card access here.
  credentials = WIFI_STORE.getCredentials();
  credentialsLoaded = true;
  LOG_INF("BWS", "Using %zu saved WiFi networks", credentials.size());
}

void BackgroundWebServer::startScan() {
  state = State::SCANNING;
  stateStartMs = millis();
  targetSsid.clear();
  targetPassword.clear();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
  LOG_INF("BWS", "Started WiFi scan");
}

void BackgroundWebServer::startConnect(const std::string& ssid, const std::string& password) {
  state = State::CONNECTING;
  stateStartMs = millis();
  targetSsid = ssid;
  targetPassword = password;
  wifiOwned = true;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  if (!targetPassword.empty()) {
    WiFi.begin(targetSsid.c_str(), targetPassword.c_str());
  } else {
    WiFi.begin(targetSsid.c_str());
  }
  LOG_INF("BWS", "Connecting to %s", targetSsid.c_str());
}

void BackgroundWebServer::startServer() {
  if (ESP.getFreeHeap() < MIN_FREE_HEAP_BYTES) {
    scheduleRetry("low heap");
    return;
  }
  if (xTaskCreate(ntpSyncTask, "TimeSyncTask", 4096, nullptr, 1, nullptr) != pdPASS) {
    LOG_ERR("BWS", "Failed to start time sync task");
  }
  if (!server) {
    server.reset(new CrossPointWebServer());
  }
  server->begin();
  if (!server->isRunning()) {
    scheduleRetry("server start failed");
    return;
  }

  char hostname[40];
  NetworkNames::getDeviceHostname(hostname, sizeof(hostname));

  if (MDNS.begin(hostname)) {
    mdnsStarted = true;
    LOG_INF("BWS", "mDNS started: http://%s.local/", hostname);
  } else {
    LOG_ERR("BWS", "mDNS failed to start");
  }

  state = State::RUNNING;
  stateStartMs = millis();
  retryAttempts = 0;
}

unsigned long BackgroundWebServer::computeBackoffMs() const {
  if (retryAttempts == 0) {
    return RETRY_BASE_MS;
  }
  const unsigned long factor = 1UL << (retryAttempts > 10 ? 10 : retryAttempts);
  const unsigned long backoff = RETRY_BASE_MS * factor;
  return backoff > RETRY_MAX_MS ? RETRY_MAX_MS : backoff;
}

void BackgroundWebServer::scheduleRetry(const char* reason) {
  if (server && server->isRunning()) {
    server->stop();
  }
  server.reset();

  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
  }

  WiFi.scanDelete();

  if (wifiOwned) {
    WiFi.disconnect();
    wifiOwned = false;
  }

  state = State::WAIT_RETRY;
  retryAttempts++;
  nextRetryMs = millis() + computeBackoffMs();
  LOG_INF("BWS", "Retry scheduled (%s) in %lu ms", reason, nextRetryMs - millis());
}

void BackgroundWebServer::stopAll() {
  if (server && server->isRunning()) {
    server->stop();
  }
  server.reset();

  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
  }

  WiFi.scanDelete();

  if (wifiOwned) {
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    wifiOwned = false;
  }
  retryAttempts = 0;
  sessionStartMs = 0;

  state = State::IDLE;
  targetSsid.clear();
  targetPassword.clear();
}

void BackgroundWebServer::resetSession() {
  retryAttempts = 0;
  sessionStartMs = millis();
}

bool BackgroundWebServer::hasSessionExpired() const {
  return sessionStartMs > 0 && (millis() - sessionStartMs >= SESSION_MAX_MS);
}

void BackgroundWebServer::loop(const bool usbConnected, const bool allowRun) {
  if (!core::FeatureModules::hasCapability(core::Capability::BackgroundServer)) {
    return;
  }

  usbConnectedCached = usbConnected;
  allowRunCached = allowRun;

  const bool effectiveAllowRun = allowRun && usbConnected;
  if (effectiveAllowRun && !lastAllowRunState) {
    allowRunStartMs = millis();
    lastAllowRunState = true;
  } else if (!effectiveAllowRun) {
    lastAllowRunState = false;
    allowRunStartMs = 0;
  }

  if (!usbConnected || !allowRun) {
    if (state != State::IDLE) {
      stopAll();
    }
    sessionBlocked = false;
    return;
  }

  if (allowRunStartMs > 0 && millis() - allowRunStartMs < ALLOW_RUN_GRACE_MS) {
    return;
  }

  if (sessionBlocked) {
    return;
  }

  if (sessionStartMs == 0) {
    resetSession();
  }
  if (hasSessionExpired()) {
    stopAll();
    sessionBlocked = true;
    return;
  }

  ensureCredentialsLoaded();
  static bool warnedEmpty = false;
  if (credentials.empty()) {
    if (!warnedEmpty) {
      LOG_INF("BWS", "No saved WiFi credentials - background server disabled");
      warnedEmpty = true;
    }
    if (state != State::IDLE) {
      stopAll();
    }
    return;
  }
  warnedEmpty = false;

  if (state == State::IDLE) {
    if (WiFi.status() == WL_CONNECTED) {
      // Use existing connection but don't claim ownership - another activity
      // may have established it. Only claim ownership when we connect ourselves.
      LOG_INF("BWS", "WiFi already connected, starting server");
      startServer();
    } else {
      LOG_INF("BWS", "WiFi not connected (status=%d), starting scan", WiFi.status());
      startScan();
    }
    return;
  }

  if (state == State::SCANNING) {
    if (millis() - stateStartMs > SCAN_TIMEOUT_MS) {
      WiFi.scanDelete();
      scheduleRetry("scan timeout");
      return;
    }
    const int16_t scanResult = WiFi.scanComplete();
    if (scanResult == WIFI_SCAN_RUNNING) {
      return;
    }
    if (scanResult == WIFI_SCAN_FAILED) {
      scheduleRetry("scan failed");
      return;
    }

    std::string ssid;
    std::string password;
    if (!findBestCredential(credentials, scanResult, ssid, password)) {
      WiFi.scanDelete();
      scheduleRetry("no saved networks");
      return;
    }

    WiFi.scanDelete();
    startConnect(ssid, password);
    return;
  }

  if (state == State::CONNECTING) {
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) {
      startServer();
      return;
    }

    if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
      scheduleRetry("connect failed");
      return;
    }

    if (millis() - stateStartMs > CONNECT_TIMEOUT_MS) {
      scheduleRetry("connect timeout");
      return;
    }

    return;
  }

  if (state == State::RUNNING) {
    if (server) {
      server->handleClient();
    }
    if (WiFi.status() != WL_CONNECTED) {
      scheduleRetry("wifi disconnected");
      return;
    }
    if (millis() - stateStartMs >= SERVER_WINDOW_MS) {
      scheduleRetry("server window expired");
      return;
    }
    return;
  }

  if (state == State::WAIT_RETRY) {
    if (static_cast<int32_t>(millis() - nextRetryMs) >= 0) {
      startScan();
    }
    return;
  }
}
