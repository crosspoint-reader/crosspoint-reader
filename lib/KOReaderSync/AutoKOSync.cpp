#include "AutoKOSync.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <ctime>

#include "KOReaderCredentialStore.h"

#ifndef SIMULATOR
#include <WiFi.h>
#include <esp_sntp.h>
#endif

// ── Static state (survives within a session; reset on power cycle) ──
unsigned long AutoKOSync::lastOpenSyncTimeMs = 0;
unsigned long AutoKOSync::lastCloseSyncTimeMs = 0;
uint8_t AutoKOSync::consecutiveFailures = 0;
std::string AutoKOSync::lastSyncedDocHash;
int AutoKOSync::lastSyncedSpine = -1;
int AutoKOSync::lastSyncedPage = -1;

// ── SD card logging ──

void AutoKOSync::logToSd(const char* message) {
  FsFile f = Storage.open("/.crosspoint/autosync.log", O_WRONLY | O_CREAT | O_APPEND);
  if (f) {
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "[%lu] ", millis());
    f.write(reinterpret_cast<const uint8_t*>(prefix), strlen(prefix));
    f.write(reinterpret_cast<const uint8_t*>(message), strlen(message));
    f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
    f.close();
  }
}

// ── Skip rules ──

bool AutoKOSync::shouldSkip(bool isOpen) {
  // 1. No KOSync credentials
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("AutoSync", "Skip: no KOSync credentials");
    logToSd("SKIP: no KOSync credentials");
    return true;
  }

  // 2. Last sync of same type too recent (open and close tracked independently)
  const unsigned long lastMs = isOpen ? lastOpenSyncTimeMs : lastCloseSyncTimeMs;
  if (lastMs > 0 && (millis() - lastMs) < MIN_SYNC_INTERVAL_MS) {
    LOG_DBG("AutoSync", "Skip: last %s sync %lums ago (min %lu)", isOpen ? "open" : "close", millis() - lastMs,
            MIN_SYNC_INTERVAL_MS);
    logToSd("SKIP: cooldown active");
    return true;
  }

  // 3. Consecutive failure backoff
  if (consecutiveFailures >= MAX_FAILURES_BEFORE_LONG_BACKOFF) {
    const unsigned long backoffMs = 3600000UL;  // 1 hour
    const unsigned long lastMs2 = std::max(lastOpenSyncTimeMs, lastCloseSyncTimeMs);
    if (lastMs2 > 0 && (millis() - lastMs2) < backoffMs) {
      LOG_DBG("AutoSync", "Skip: backoff after %d failures", consecutiveFailures);
      logToSd("SKIP: failure backoff");
      return true;
    }
    LOG_DBG("AutoSync", "Backoff elapsed, allowing retry");
    logToSd("Backoff elapsed, retrying");
  }

  logToSd(isOpen ? "shouldSkip: PASS (open)" : "shouldSkip: PASS (close)");
  return false;
}

void AutoKOSync::recordSuccess(bool isOpen) {
  if (isOpen) {
    lastOpenSyncTimeMs = millis();
  } else {
    lastCloseSyncTimeMs = millis();
  }
  consecutiveFailures = 0;
  LOG_DBG("AutoSync", "Success recorded (%s) at %lu", isOpen ? "open" : "close", millis());
}

void AutoKOSync::recordFailure() {
  // On failure, cool down both directions — WiFi/server is bad
  lastOpenSyncTimeMs = millis();
  lastCloseSyncTimeMs = millis();
  consecutiveFailures++;
  LOG_DBG("AutoSync", "Failure %d recorded", consecutiveFailures);
}

void AutoKOSync::recordPosition(const std::string& documentHash, int spineIndex, int page) {
  lastSyncedDocHash = documentHash;
  lastSyncedSpine = spineIndex;
  lastSyncedPage = page;
}

bool AutoKOSync::hasPositionChanged(const std::string& documentHash, int spineIndex, int page) {
  if (lastSyncedDocHash != documentHash) return true;
  return (lastSyncedSpine != spineIndex || lastSyncedPage != page);
}

// ── WiFi ──

bool AutoKOSync::connectWifi(const std::string& ssid, const std::string& password, uint32_t timeoutMs) {
#ifdef SIMULATOR
  LOG_DBG("AutoSync", "Simulator: WiFi connect stub (always succeeds)");
  logToSd("WiFi: simulator stub OK");
  return true;
#else
  if (ssid.empty()) {
    LOG_DBG("AutoSync", "No SSID provided");
    logToSd("WiFi: no SSID");
    return false;
  }

  char buf[96];
  snprintf(buf, sizeof(buf), "WiFi: connecting to '%s'...", ssid.c_str());
  logToSd(buf);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);

  if (password.empty()) {
    WiFi.begin(ssid.c_str());
  } else {
    WiFi.begin(ssid.c_str(), password.c_str());
  }

  const unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    const auto status = WiFi.status();
    if (status == WL_CONNECTED) {
      snprintf(buf, sizeof(buf), "WiFi: connected in %lums", millis() - start);
      logToSd(buf);
      return true;
    }
    if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
      snprintf(buf, sizeof(buf), "WiFi: FAILED (status %d)", status);
      logToSd(buf);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      return false;
    }
    delay(100);
  }

  snprintf(buf, sizeof(buf), "WiFi: TIMEOUT after %lums", timeoutMs);
  logToSd(buf);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return false;
#endif
}

void AutoKOSync::disconnectWifi() {
#ifdef SIMULATOR
  LOG_DBG("AutoSync", "Simulator: WiFi disconnect stub");
#else
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
  LOG_DBG("AutoSync", "WiFi disconnected");
#endif
}

void AutoKOSync::syncNtpIfNeeded() {
#ifdef SIMULATOR
  return;  // Host time is already valid
#else
  // Skip NTP if system time looks valid (year > 2025)
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  if (timeinfo.tm_year > 125) {  // tm_year is years since 1900; 125 = year 2025
    LOG_DBG("AutoSync", "NTP skip: time already valid (%d)", timeinfo.tm_year + 1900);
    return;
  }

  // Quick NTP sync with short timeout
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  int retry = 0;
  const int maxRetries = 20;  // 2 seconds max
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < maxRetries) {
    delay(100);
    retry++;
  }
  LOG_DBG("AutoSync", "NTP %s (%d retries)", retry < maxRetries ? "synced" : "timeout", retry);
#endif
}

// ── Document hash ──

std::string AutoKOSync::computeDocHash(const std::string& bookPath) {
  if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
    return KOReaderDocumentId::calculateFromFilename(bookPath);
  }
  return KOReaderDocumentId::calculate(bookPath);
}

// ── Sync on open ──

AutoKOSync::OpenResult AutoKOSync::syncOnOpen(const std::string& bookPath, const std::string& wifiSsid,
                                              const std::string& wifiPassword) {
  OpenResult result;

  logToSd("=== SYNC ON OPEN ===");

  if (shouldSkip(true)) return result;

  char buf[128];
  snprintf(buf, sizeof(buf), "Book: %s", bookPath.c_str());
  logToSd(buf);

  // Connect WiFi (fast timeout for open — fail quickly when away from home)
  if (!connectWifi(wifiSsid, wifiPassword, WIFI_TIMEOUT_OPEN_MS)) {
    logToSd("FAIL: WiFi connect failed");
    recordFailure();
    result.status = SyncStatus::WIFI_FAILED;
    return result;
  }

  syncNtpIfNeeded();

  // Compute document hash
  const std::string docHash = computeDocHash(bookPath);
  if (docHash.empty()) {
    LOG_ERR("AutoSync", "Failed to compute document hash");
    logToSd("FAIL: empty document hash");
    disconnectWifi();
    recordFailure();
    result.status = SyncStatus::SYNC_FAILED;
    return result;
  }

  snprintf(buf, sizeof(buf), "Hash: %s", docHash.c_str());
  logToSd(buf);

  // Fetch remote progress
  KOReaderProgress remoteProgress;
  const auto getResult = KOReaderSyncClient::getProgress(docHash, remoteProgress);

  if (getResult == KOReaderSyncClient::OK) {
    result.hasRemote = true;
    result.remoteProgress = remoteProgress;
    snprintf(buf, sizeof(buf), "Remote: %.1f%% from %s", remoteProgress.percentage * 100,
             remoteProgress.device.c_str());
    logToSd(buf);
  } else if (getResult == KOReaderSyncClient::NOT_FOUND) {
    logToSd("No remote progress (404)");
  } else {
    snprintf(buf, sizeof(buf), "FAIL: GET error %d (HTTP %d)", getResult, KOReaderSyncClient::lastHttpCode);
    logToSd(buf);
    disconnectWifi();
    recordFailure();
    result.status = SyncStatus::SYNC_FAILED;
    return result;
  }

  disconnectWifi();
  recordSuccess(true);
  logToSd("Open sync complete");
  result.status = SyncStatus::SUCCESS;
  return result;
}

// ── Sync on close ──

AutoKOSync::SyncStatus AutoKOSync::syncOnClose(const ClosePayload& payload) {
  logToSd("=== SYNC ON CLOSE ===");

  if (!payload.valid) {
    logToSd("SKIP: payload invalid");
    return SyncStatus::SKIPPED;
  }
  if (shouldSkip(false)) return SyncStatus::SKIPPED;

  // Check if position actually changed since last sync
  if (!hasPositionChanged(payload.documentHash, 0, 0)) {
    logToSd("SKIP: position unchanged");
    return SyncStatus::SKIPPED;
  }

  char buf[128];
  snprintf(buf, sizeof(buf), "Book: %s (%.1f%%)", payload.epubPath.c_str(), payload.localPercentage * 100);
  logToSd(buf);

  // Connect WiFi (longer timeout for close — user is done reading)
  if (!connectWifi(payload.wifiSsid, payload.wifiPassword, WIFI_TIMEOUT_CLOSE_MS)) {
    logToSd("FAIL: WiFi connect failed");
    recordFailure();
    return SyncStatus::WIFI_FAILED;
  }

  syncNtpIfNeeded();

  // Fetch remote to compare direction
  KOReaderProgress remoteProgress;
  const auto getResult = KOReaderSyncClient::getProgress(payload.documentHash, remoteProgress);

  bool shouldUpload = false;

  if (getResult == KOReaderSyncClient::OK) {
    snprintf(buf, sizeof(buf), "Remote: %.1f%%", remoteProgress.percentage * 100);
    logToSd(buf);
    // Only upload if local is ahead
    if (payload.localPercentage > remoteProgress.percentage + 0.001f) {
      shouldUpload = true;
      logToSd("Local ahead, will upload");
    } else {
      logToSd("Remote same/ahead, skipping upload");
    }
  } else if (getResult == KOReaderSyncClient::NOT_FOUND) {
    shouldUpload = true;
    logToSd("No remote (404), will upload");
  } else {
    snprintf(buf, sizeof(buf), "FAIL: GET error %d (HTTP %d)", getResult, KOReaderSyncClient::lastHttpCode);
    logToSd(buf);
    disconnectWifi();
    recordFailure();
    return SyncStatus::SYNC_FAILED;
  }

  if (shouldUpload) {
    KOReaderProgress progress;
    progress.document = payload.documentHash;
    progress.progress = payload.localKoPos.xpath;
    progress.percentage = payload.localPercentage;

    const auto putResult = KOReaderSyncClient::updateProgress(progress);
    if (putResult != KOReaderSyncClient::OK) {
      snprintf(buf, sizeof(buf), "FAIL: PUT error %d (HTTP %d)", putResult, KOReaderSyncClient::lastHttpCode);
      logToSd(buf);
      disconnectWifi();
      recordFailure();
      return SyncStatus::SYNC_FAILED;
    }
    logToSd("Upload SUCCESS");
  }

  disconnectWifi();
  recordSuccess(false);
  recordPosition(payload.documentHash, 0, static_cast<int>(payload.localPercentage * 10000));
  logToSd("Close sync complete");
  return SyncStatus::SUCCESS;
}
