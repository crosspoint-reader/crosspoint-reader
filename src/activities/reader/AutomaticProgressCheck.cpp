#include "AutomaticProgressCheck.h"

#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <Logging.h>

#include <algorithm>

#include "AutomaticWifiConnectionPolicy.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "WifiCredentialStore.h"

namespace {

// Headless, bounded WiFi connect: try each saved credential once, then give up.
// Mirrors WifiSelectionActivity's auto-connect path but blocks (this runs in a
// dedicated task, not the activity loop).
bool connectHeadless() {
  if (WiFi.status() == WL_CONNECTED) return true;

  const uint32_t deadline = millis() + AutomaticWifiConnectionPolicy::BACKGROUND_TIMEOUT_MS;

  WiFi.persistent(false);  // Credentials are managed by WifiCredentialStore
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);  // Abort any SDK auto-connect and clear NVS SSID
  vTaskDelay(pdMS_TO_TICKS(100));

  for (size_t i = 0; i < WIFI_STORE.getCredentialCount(); i++) {
    const auto cred = WIFI_STORE.getCredentialAt(i);
    if (!cred) continue;

    const int32_t remaining = static_cast<int32_t>(deadline - millis());
    if (remaining <= 0) break;

    LOG_DBG("KOSync", "Automatic check: trying saved network %s", cred->ssid.c_str());
    if (cred->password.empty()) {
      WiFi.begin(cred->ssid.c_str());
    } else {
      WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
    }

    const uint32_t attemptTimeoutMs = std::min<uint32_t>(
        static_cast<uint32_t>(remaining), AutomaticWifiConnectionPolicy::FALLBACK_ATTEMPT_TIMEOUT_MS);
    const uint32_t attemptStartedAt = millis();
    while (millis() - attemptStartedAt < attemptTimeoutMs) {
      if (WiFi.status() == WL_CONNECTED) {
        LOG_DBG("KOSync", "Automatic check: connected to %s", cred->ssid.c_str());
        return true;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    WiFi.disconnect();
  }
  return WiFi.status() == WL_CONNECTED;
}

}  // namespace

AutomaticProgressCheck::~AutomaticProgressCheck() {
  if (taskHandle_ && status_.load(std::memory_order_acquire) == Status::RUNNING) {
    vTaskDelete(taskHandle_);
  }
  taskHandle_ = nullptr;
}

bool AutomaticProgressCheck::start(const std::string& epubPath) {
  if (isRunning()) return false;
  if (!KOREADER_STORE.hasCredentials()) return false;

  epubPath_ = epubPath;
  remoteProgress_ = {};
  error_ = KOReaderSyncClient::OK;
  status_.store(Status::RUNNING, std::memory_order_release);

  if (xTaskCreatePinnedToCore(&taskTrampoline, "AutoProgressCheck", 8192, this, 1, &taskHandle_, 0) != pdPASS) {
    LOG_ERR("KOSync", "Failed to create automatic progress check task");
    status_.store(Status::DONE_ERROR, std::memory_order_release);
    error_ = KOReaderSyncClient::NETWORK_ERROR;
    taskHandle_ = nullptr;
    return false;
  }
  return true;
}

void AutomaticProgressCheck::taskTrampoline(void* arg) {
  auto* self = static_cast<AutomaticProgressCheck*>(arg);
  self->run();
  vTaskDelete(nullptr);  // never touches self after this point
}

void AutomaticProgressCheck::run() {
  if (!connectHeadless()) {
    LOG_DBG("KOSync", "Automatic check: WiFi unavailable");
    error_ = KOReaderSyncClient::NETWORK_ERROR;
    status_.store(Status::DONE_ERROR, std::memory_order_release);
    return;
  }

  const std::string hash = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
                               ? KOReaderDocumentId::calculateFromFilename(epubPath_)
                               : KOReaderDocumentId::calculate(epubPath_);
  if (hash.empty()) {
    LOG_ERR("KOSync", "Automatic check: document hash failed");
    error_ = KOReaderSyncClient::NETWORK_ERROR;
    status_.store(Status::DONE_ERROR, std::memory_order_release);
    return;
  }

  KOReaderProgress progress;
  const auto result = KOReaderSyncClient::getProgress(hash, progress);
  error_ = result;

  // Drop the radio; the reader resumes normal low-power operation.
  WiFi.disconnect(true, false);

  if (result == KOReaderSyncClient::OK) {
    remoteProgress_ = std::move(progress);
    status_.store(Status::DONE_OK, std::memory_order_release);
  } else if (result == KOReaderSyncClient::NOT_FOUND) {
    status_.store(Status::DONE_NOT_FOUND, std::memory_order_release);
  } else {
    LOG_DBG("KOSync", "Automatic check: getProgress failed (%d)", static_cast<int>(result));
    status_.store(Status::DONE_ERROR, std::memory_order_release);
  }
}
