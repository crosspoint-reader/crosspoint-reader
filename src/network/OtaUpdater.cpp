#include "OtaUpdater.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "SpiBusMutex.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"

namespace {
constexpr char releaseChannelUrl[] =
    "https://api.github.com/repos/Unintendedsideeffects/crosspoint-reader/releases/tags/stable";
constexpr char nightlyChannelUrl[] =
    "https://api.github.com/repos/Unintendedsideeffects/crosspoint-reader/releases/tags/nightly";
constexpr char latestChannelUrl[] =
    "https://api.github.com/repos/Unintendedsideeffects/crosspoint-reader/releases/tags/latest";
constexpr char resetChannelUrl[] =
    "https://api.github.com/repos/Unintendedsideeffects/crosspoint-reader/releases/tags/reset";
// Override at build time via -DFEATURE_STORE_CATALOG_URL='"..."' in platformio.ini build_flags.
#ifndef FEATURE_STORE_CATALOG_URL
#define FEATURE_STORE_CATALOG_URL                                                                  \
  "https://raw.githubusercontent.com/Unintendedsideeffects/crosspoint-reader/fork-drift/docs/ota/" \
  "feature-store-catalog.json"
#endif
constexpr char featureStoreCatalogUrl[] = FEATURE_STORE_CATALOG_URL;
constexpr char expectedBoard[] = "esp32c3";
constexpr char factoryResetMarkerFile[] = "/.factory-reset-pending";
constexpr uint32_t otaNoProgressTimeoutMs = 45000;
constexpr uint8_t otaMaxAttempts = 3;
constexpr uint32_t otaRetryBackoffBaseMs = 1000;

constexpr size_t sha256DigestBytes = 32;

bool otaCanceled(const std::atomic<bool>* cancelFlag) {
  return cancelFlag != nullptr && cancelFlag->load(std::memory_order_acquire);
}

int hexNibble(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseSha256Hex(const String& checksum, uint8_t outDigest[sha256DigestBytes]) {
  String normalized = checksum;
  normalized.trim();
  if (normalized.startsWith("sha256:")) {
    normalized = normalized.substring(7);
  } else if (normalized.startsWith("SHA256:")) {
    normalized = normalized.substring(7);
  }
  normalized.trim();

  if (normalized.length() != static_cast<int>(sha256DigestBytes * 2)) {
    return false;
  }

  for (size_t i = 0; i < sha256DigestBytes; ++i) {
    const int hi = hexNibble(normalized[2 * i]);
    const int lo = hexNibble(normalized[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    outDigest[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

String digestToHex(const uint8_t digest[sha256DigestBytes]) {
  static constexpr char kHex[] = "0123456789abcdef";
  char out[sha256DigestBytes * 2 + 1] = {0};
  for (size_t i = 0; i < sha256DigestBytes; ++i) {
    out[2 * i] = kHex[digest[i] >> 4];
    out[2 * i + 1] = kHex[digest[i] & 0x0F];
  }
  return String(out);
}

bool calculatePartitionSha256(const esp_partition_t* partition, size_t imageSize,
                              uint8_t outDigest[sha256DigestBytes]) {
  if (partition == nullptr || imageSize == 0) {
    return false;
  }

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  if (mbedtls_sha256_starts_ret(&ctx, 0) != 0) {
    mbedtls_sha256_free(&ctx);
    return false;
  }

  constexpr size_t readChunkSize = 1024;
  uint8_t chunkBuf[readChunkSize];
  size_t offset = 0;
  while (offset < imageSize) {
    const size_t toRead = std::min(readChunkSize, imageSize - offset);
    const esp_err_t readErr = esp_partition_read(partition, offset, chunkBuf, toRead);
    if (readErr != ESP_OK) {
      mbedtls_sha256_free(&ctx);
      return false;
    }
    if (mbedtls_sha256_update_ret(&ctx, chunkBuf, toRead) != 0) {
      mbedtls_sha256_free(&ctx);
      return false;
    }
    offset += toRead;
  }

  const bool ok = mbedtls_sha256_finish_ret(&ctx, outDigest) == 0;
  mbedtls_sha256_free(&ctx);
  return ok;
}

bool verifyPartitionChecksum(const esp_partition_t* partition, const String& expectedChecksum, size_t imageSize,
                             String& outError) {
  uint8_t expectedDigest[sha256DigestBytes] = {0};
  if (!parseSha256Hex(expectedChecksum, expectedDigest)) {
    outError = "Invalid bundle checksum metadata";
    return false;
  }

  uint8_t actualDigest[sha256DigestBytes] = {0};
  if (!calculatePartitionSha256(partition, imageSize, actualDigest)) {
    outError = "Failed to verify installed image checksum";
    return false;
  }

  if (memcmp(expectedDigest, actualDigest, sha256DigestBytes) != 0) {
    outError = "Installed image checksum mismatch";
    LOG_ERR("OTA", "Checksum mismatch. expected=%s actual=%s", expectedChecksum.c_str(),
            digestToHex(actualDigest).c_str());
    return false;
  }

  return true;
}

bool markFactoryResetPending() {
  FsFile markerFile;
  {
    SpiBusMutex::Guard guard;
    if (!Storage.openFileForWrite("OTA", factoryResetMarkerFile, markerFile)) {
      LOG_ERR("OTA", "Failed to create factory reset marker: %s", factoryResetMarkerFile);
      return false;
    }
    static constexpr uint8_t markerByte = 1;
    markerFile.write(&markerByte, sizeof(markerByte));
    markerFile.close();
  }

  LOG_INF("OTA", "Factory reset marker created: %s", factoryResetMarkerFile);
  return true;
}

bool parseSemver(const std::string& version, int& major, int& minor, int& patch) {
  const char* versionStr = version.c_str();
  if (versionStr[0] == 'v' || versionStr[0] == 'V') {
    versionStr += 1;
  }
  return sscanf(versionStr, "%d.%d.%d", &major, &minor, &patch) == 3;
}

// "12345-dev" → commit count format. Returns true and sets count on match.
bool parseCommitDev(const std::string& v, unsigned long& count) {
  if (v.size() < 5 || v.compare(v.size() - 4, 4, "-dev") != 0) return false;
  const auto numStr = v.substr(0, v.size() - 4);
  if (numStr.empty()) return false;
  for (const char c : numStr) {
    if (c < '0' || c > '9') return false;
  }
  return sscanf(numStr.c_str(), "%lu", &count) == 1;
}

// "20240218" → YYYYMMDD date format. Returns true and sets date on match.
bool parseBuildDate(const std::string& v, unsigned long& date) {
  if (v.size() != 8) return false;
  for (const char c : v) {
    if (c < '0' || c > '9') return false;
  }
  if (sscanf(v.c_str(), "%lu", &date) != 1) return false;
  // Sanity check: must look like a real date after year 2020
  return date >= 20200101UL && date <= 29991231UL;
}

/*
 * When esp_crt_bundle.h included, it is pointing wrong header file
 * which is something under WifiClientSecure because of our framework based on arduno platform.
 * To manage this obstacle, don't include anything, just extern and it will point correct one.
 */
extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}

// Buffer accumulated by the HTTP event handler during a JSON fetch.
// Passed via user_data so each fetch call has its own isolated state.
struct HttpBuf {
  char* data = nullptr;
  size_t len = 0;
};

esp_err_t http_client_set_header_cb(esp_http_client_handle_t http_client) {
  return esp_http_client_set_header(http_client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
}

esp_err_t event_handler(esp_http_client_event_t* event) {
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;

  // Append this chunk to the caller-supplied buffer.
  // realloc(NULL, n) behaves as malloc(n), so this handles the first call correctly.
  auto* buf = static_cast<HttpBuf*>(event->user_data);
  const size_t data_len = static_cast<size_t>(event->data_len);
  char* new_data = static_cast<char*>(realloc(buf->data, buf->len + data_len + 1));
  if (new_data == NULL) {
    LOG_ERR("OTA", "Failed to allocate HTTP response buffer (%zu bytes)", buf->len + data_len + 1);
    return ESP_ERR_NO_MEM;
  }

  buf->data = new_data;
  memcpy(buf->data + buf->len, event->data, data_len);
  buf->len += data_len;
  buf->data[buf->len] = '\0';
  return ESP_OK;
}

/**
 * Fetch JSON from a URL into ArduinoJson doc.
 * Returns OtaUpdater::OK on success, or an error code.
 */
OtaUpdater::OtaUpdaterError fetchReleaseJson(const char* url, JsonDocument& doc, const JsonDocument& filter) {
  HttpBuf httpBuf;

  esp_http_client_config_t client_config = {
      .url = url,
      .timeout_ms = 15000,
      .event_handler = event_handler,
      .buffer_size = 8192,
      .buffer_size_tx = 8192,
      .user_data = &httpBuf,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  struct localBufCleaner {
    HttpBuf* buf;
    ~localBufCleaner() {
      if (buf->data) {
        free(buf->data);
        buf->data = nullptr;
      }
    }
  } cleaner = {&httpBuf};

  esp_http_client_handle_t client_handle = esp_http_client_init(&client_config);
  if (!client_handle) {
    LOG_ERR("OTA", "HTTP Client Handle Failed");
    return OtaUpdater::INTERNAL_UPDATE_ERROR;
  }

  esp_err_t esp_err = esp_http_client_set_header(client_handle, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_set_header Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return OtaUpdater::INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_perform(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_perform Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return OtaUpdater::HTTP_ERROR;
  }

  esp_err = esp_http_client_cleanup(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_cleanup Failed : %s", esp_err_to_name(esp_err));
    return OtaUpdater::INTERNAL_UPDATE_ERROR;
  }

  const DeserializationError error = deserializeJson(doc, httpBuf.data, DeserializationOption::Filter(filter));
  if (error) {
    LOG_ERR("OTA", "JSON parse failed: %s", error.c_str());
    return OtaUpdater::JSON_PARSE_ERROR;
  }

  return OtaUpdater::OK;
}

size_t getMaxOtaPartitionSize() {
  const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
  if (partition) {
    return partition->size;
  }
  return 0;
}

String describeCheckForUpdateError(const OtaUpdater::OtaUpdaterError error) {
  switch (error) {
    case OtaUpdater::HTTP_ERROR:
      return "Unable to reach update server";
    case OtaUpdater::JSON_PARSE_ERROR:
      return "Invalid update metadata";
    case OtaUpdater::UPDATE_OLDER_ERROR:
      return "No newer update available";
    case OtaUpdater::INTERNAL_UPDATE_ERROR:
      return "Update check failed";
    case OtaUpdater::OOM_ERROR:
      return "Out of memory during update check";
    case OtaUpdater::NO_UPDATE:
      return "No update package found";
    case OtaUpdater::OK:
    default:
      return "";
  }
}

} /* namespace */

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  if (otaCanceled(cancelRequested)) {
    lastError = "Update canceled";
    return INTERNAL_UPDATE_ERROR;
  }

  updateAvailable = false;
  factoryResetOnInstall = false;
  latestVersion.clear();
  otaUrl.clear();
  lastError.clear();
  otaSize = 0;
  totalSize = 0;

  // If a feature store bundle was selected, use its URL directly
  if (!selectedBundleId.isEmpty()) {
    for (const auto& entry : featureStoreEntries) {
      if (entry.id == selectedBundleId) {
        otaUrl = entry.downloadUrl.c_str();
        otaSize = entry.binarySize;
        totalSize = entry.binarySize;
        latestVersion = entry.version.c_str();
        updateAvailable = true;
        LOG_DBG("OTA", "Using feature store bundle: %s", selectedBundleId.c_str());
        return OK;
      }
    }
    lastError = BUNDLE_UNAVAILABLE_ERROR;
    return INTERNAL_UPDATE_ERROR;
  }

  // Fall back to channel-based OTA
  const char* releaseUrl = releaseChannelUrl;
  if (SETTINGS.releaseChannel == CrossPointSettings::RELEASE_NIGHTLY) {
    releaseUrl = nightlyChannelUrl;
  } else if (SETTINGS.releaseChannel == CrossPointSettings::RELEASE_LATEST_SUCCESSFUL) {
    releaseUrl = latestChannelUrl;
  } else if (SETTINGS.releaseChannel == CrossPointSettings::RELEASE_LATEST_SUCCESSFUL_FACTORY_RESET) {
    releaseUrl = resetChannelUrl;
    factoryResetOnInstall = true;
  }

  JsonDocument filter;
  JsonDocument doc;
  filter["tag_name"] = true;
  filter["name"] = true;  // release title — CI sets this to the build version string
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  filter["assets"][0]["size"] = true;

  auto res = fetchReleaseJson(releaseUrl, doc, filter);
  if (res != OK && releaseUrl == releaseChannelUrl && releaseChannelUrl != latestChannelUrl) {
    LOG_WRN("OTA", "Release channel unavailable, falling back to latest");
    res = fetchReleaseJson(latestChannelUrl, doc, filter);
  }
  if (res != OK) {
    lastError = describeCheckForUpdateError(res);
    return res;
  }

  if (!doc["tag_name"].is<std::string>()) {
    LOG_ERR("OTA", "No tag_name found");
    lastError = "Update metadata missing tag";
    return JSON_PARSE_ERROR;
  }

  if (!doc["assets"].is<JsonArray>()) {
    LOG_ERR("OTA", "No assets found");
    lastError = "Update metadata missing assets";
    return JSON_PARSE_ERROR;
  }

  // Use the release title (name) as the build version identifier — it carries
  // meaningful version strings like "12345-dev", "20240218", or "1.0.0".
  // Fall back to tag_name for older releases that predate this convention.
  const std::string releaseName = doc["name"] | "";
  latestVersion = releaseName.empty() ? doc["tag_name"].as<std::string>() : releaseName;

  for (int i = 0; i < doc["assets"].size(); i++) {
    if (doc["assets"][i]["name"] == "firmware.bin") {
      otaUrl = doc["assets"][i]["browser_download_url"].as<std::string>();
      otaSize = doc["assets"][i]["size"].as<size_t>();
      totalSize = otaSize;
      updateAvailable = true;
      break;
    }
  }

  if (!updateAvailable) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    lastError = "Release missing firmware package";
    return NO_UPDATE;
  }

  LOG_DBG("OTA", "Found update: %s", latestVersion.c_str());
  return OK;
}

bool OtaUpdater::loadFeatureStoreCatalog() {
  featureStoreEntries.clear();
  lastError.clear();

  JsonDocument filter;
  JsonDocument doc;
  filter["bundles"][0]["id"] = true;
  filter["bundles"][0]["displayName"] = true;
  filter["bundles"][0]["version"] = true;
  filter["bundles"][0]["board"] = true;
  filter["bundles"][0]["featureFlags"] = true;
  filter["bundles"][0]["downloadUrl"] = true;
  filter["bundles"][0]["checksum"] = true;
  filter["bundles"][0]["binarySize"] = true;

  const auto res = fetchReleaseJson(featureStoreCatalogUrl, doc, filter);
  if (res != OK) {
    lastError = CATALOG_UNAVAILABLE_ERROR;
    return false;
  }

  if (!doc["bundles"].is<JsonArray>()) {
    lastError = CATALOG_UNAVAILABLE_ERROR;
    return false;
  }

  const size_t maxPartitionSize = getMaxOtaPartitionSize();

  for (const auto& bundle : doc["bundles"].as<JsonArray>()) {
    FeatureStoreEntry entry;
    entry.id = bundle["id"].as<const char*>();
    entry.displayName = bundle["displayName"].as<const char*>();
    entry.version = bundle["version"].as<const char*>();
    entry.featureFlags = bundle["featureFlags"].as<const char*>();
    entry.downloadUrl = bundle["downloadUrl"].as<const char*>();
    entry.checksum = bundle["checksum"] | "";
    entry.binarySize = bundle["binarySize"] | 0;

    const char* board = bundle["board"] | "";
    if (strcmp(board, expectedBoard) != 0) {
      entry.compatible = false;
      entry.compatibilityError = "Requires board: " + String(board);
    } else if (entry.binarySize > 0 && maxPartitionSize > 0 && entry.binarySize > maxPartitionSize) {
      entry.compatible = false;
      entry.compatibilityError = "Binary too large for OTA partition";
    }

    featureStoreEntries.push_back(entry);
  }

  return !featureStoreEntries.empty();
}

bool OtaUpdater::hasFeatureStoreCatalog() const { return !featureStoreEntries.empty(); }

const std::vector<OtaUpdater::FeatureStoreEntry>& OtaUpdater::getFeatureStoreEntries() const {
  return featureStoreEntries;
}

bool OtaUpdater::selectFeatureStoreBundleByIndex(size_t index) {
  if (index >= featureStoreEntries.size()) {
    return false;
  }

  const auto& entry = featureStoreEntries[index];
  if (!entry.compatible) {
    lastError = INCOMPATIBLE_BUNDLE_ERROR;
    return false;
  }

  selectedBundleId = entry.id;
  selectedFeatureFlags = entry.featureFlags;
  selectedChecksum = entry.checksum;

  // Persist selection
  strncpy(SETTINGS.selectedOtaBundle, entry.id.c_str(), sizeof(SETTINGS.selectedOtaBundle) - 1);
  SETTINGS.selectedOtaBundle[sizeof(SETTINGS.selectedOtaBundle) - 1] = '\0';
  if (!SETTINGS.saveToFile()) {
    LOG_WRN("OTA", "Failed to persist selected bundle ID to SD card");
  }

  return true;
}

const String& OtaUpdater::getLastError() const { return lastError; }

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty()) {
    return false;
  }

  // Identical strings → same build, nothing to do.
  if (latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  const std::string currentV(CROSSPOINT_VERSION);
  const std::string& latestV = latestVersion;

  // --- Commit-dev format: "12345-dev" ---
  unsigned long latestCommit = 0, currentCommit = 0;
  const bool latestIsCommitDev = parseCommitDev(latestV, latestCommit);
  const bool currentIsCommitDev = parseCommitDev(currentV, currentCommit);
  if (latestIsCommitDev && currentIsCommitDev) {
    return latestCommit > currentCommit;
  }

  // --- Date format: "YYYYMMDD" ---
  unsigned long latestDate = 0, currentDate = 0;
  const bool latestIsDate = parseBuildDate(latestV, latestDate);
  const bool currentIsDate = parseBuildDate(currentV, currentDate);
  if (latestIsDate && currentIsDate) {
    return latestDate > currentDate;
  }

  // --- Semver: "1.2.3" or "v1.2.3" ---
  int latestMaj = 0, latestMin = 0, latestPat = 0;
  int currentMaj = 0, currentMin = 0, currentPat = 0;
  const bool latestIsSemver = parseSemver(latestV, latestMaj, latestMin, latestPat);
  const bool currentIsSemver = parseSemver(currentV, currentMaj, currentMin, currentPat);
  if (latestIsSemver && currentIsSemver) {
    if (latestMaj != currentMaj) return latestMaj > currentMaj;
    if (latestMin != currentMin) return latestMin > currentMin;
    if (latestPat != currentPat) return latestPat > currentPat;
    // Equal semver segments: still offer the update if currently on a pre-release
    // (e.g. RC build getting the final stable, or dev/custom suffix builds).
    return strstr(currentV.c_str(), "-") != nullptr;
  }

  // --- Cross-format or unrecognised tokens (e.g. feature-store "latest"/"nightly") ---
  // Formats differ → device is on one version scheme, server returned another.
  // This is a deliberate channel switch (e.g. semver stable → date-format nightly).
  // Version strings are not comparable as scalars, so we allow the install.
  // If gating is required, the calling UI layer should confirm with the user before
  // invoking installUpdate() when isUpdateNewer() returns true across channel types.
  return true;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate() {
  lastError.clear();
  processedSize = 0;
  totalSize = otaSize;
  render = false;

  if (otaCanceled(cancelRequested)) {
    lastError = "Update canceled";
    return INTERNAL_UPDATE_ERROR;
  }

  if (!isUpdateNewer()) {
    lastError = "No newer update available";
    return UPDATE_OLDER_ERROR;
  }

  esp_err_t esp_err;

  esp_http_client_config_t client_config = {
      .url = otaUrl.c_str(),
      .timeout_ms = 15000,
      /* Default HTTP client buffer size 512 byte only
       * not sufficent to handle URL redirection cases or
       * parsing of large HTTP headers.
       */
      .buffer_size = 8192,
      .buffer_size_tx = 8192,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  esp_https_ota_config_t ota_config = {
      .http_config = &client_config,
      .http_client_init_cb = http_client_set_header_cb,
  };

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);
  struct WifiPsRestore {
    ~WifiPsRestore() { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); }
  } wifiPsRestore;

  OtaUpdaterError finalError = INTERNAL_UPDATE_ERROR;
  String finalMessage = "Update failed";

  for (uint8_t attempt = 1; attempt <= otaMaxAttempts; ++attempt) {
    if (otaCanceled(cancelRequested)) {
      lastError = "Update canceled";
      return INTERNAL_UPDATE_ERROR;
    }

    esp_https_ota_handle_t ota_handle = NULL;
    const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
    bool attemptFailed = false;
    bool attemptRetryable = false;
    OtaUpdaterError attemptError = INTERNAL_UPDATE_ERROR;
    String attemptMessage;

    auto abortOta = [&]() {
      if (ota_handle != NULL) {
        const esp_err_t abortErr = esp_https_ota_abort(ota_handle);
        if (abortErr != ESP_OK) {
          LOG_WRN("OTA", "esp_https_ota_abort failed: %s", esp_err_to_name(abortErr));
        }
        ota_handle = NULL;
      }
    };

    auto canRetry = [&]() {
      return attempt < otaMaxAttempts && WiFi.status() == WL_CONNECTED && !otaCanceled(cancelRequested);
    };

    processedSize = 0;
    esp_err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (esp_err != ESP_OK) {
      LOG_ERR("OTA", "HTTP OTA Begin failed (attempt %u/%u): %s", static_cast<unsigned int>(attempt),
              static_cast<unsigned int>(otaMaxAttempts), esp_err_to_name(esp_err));
      attemptFailed = true;
      attemptError = INTERNAL_UPDATE_ERROR;
      attemptMessage = "Failed to start OTA session";
      attemptRetryable = canRetry();
    }

    if (!attemptFailed) {
      const int discoveredImageSize = esp_https_ota_get_image_size(ota_handle);
      if (discoveredImageSize > 0) {
        totalSize = static_cast<size_t>(discoveredImageSize);
      }
    }

    if (!attemptFailed) {
      size_t lastProgressBytes = 0;
      unsigned long lastProgressAt = millis();

      do {
        if (otaCanceled(cancelRequested)) {
          attemptFailed = true;
          attemptError = INTERNAL_UPDATE_ERROR;
          attemptMessage = "Update canceled";
          attemptRetryable = false;
          abortOta();
          break;
        }

        if (WiFi.status() != WL_CONNECTED) {
          LOG_ERR("OTA", "WiFi disconnected during OTA");
          attemptFailed = true;
          attemptError = HTTP_ERROR;
          attemptMessage = "WiFi disconnected during update";
          attemptRetryable = false;
          abortOta();
          break;
        }

        esp_err = esp_https_ota_perform(ota_handle);
        const int imageLenRead = esp_https_ota_get_image_len_read(ota_handle);
        if (imageLenRead >= 0) {
          processedSize = static_cast<size_t>(imageLenRead);
        }
        const int discoveredImageSize = esp_https_ota_get_image_size(ota_handle);
        if (discoveredImageSize > 0) {
          totalSize = static_cast<size_t>(discoveredImageSize);
        }
        if (processedSize > lastProgressBytes) {
          lastProgressBytes = processedSize;
          lastProgressAt = millis();
        } else if (esp_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS && (millis() - lastProgressAt) > otaNoProgressTimeoutMs) {
          LOG_ERR("OTA", "OTA stalled for >%lu ms without progress",
                  static_cast<unsigned long>(otaNoProgressTimeoutMs));
          attemptFailed = true;
          attemptError = HTTP_ERROR;
          attemptMessage = "Update stalled; check network and retry";
          attemptRetryable = canRetry();
          abortOta();
          break;
        }

        /* Signal for OtaUpdateActivity */
        render = true;
        vTaskDelay(10 / portTICK_PERIOD_MS);
      } while (esp_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);
    }

    if (!attemptFailed && esp_err != ESP_OK) {
      LOG_ERR("OTA", "esp_https_ota_perform failed (attempt %u/%u): %s", static_cast<unsigned int>(attempt),
              static_cast<unsigned int>(otaMaxAttempts), esp_err_to_name(esp_err));
      attemptFailed = true;
      attemptError = HTTP_ERROR;
      attemptMessage = "Update download failed";
      attemptRetryable = canRetry();
      abortOta();
    }

    if (!attemptFailed && !esp_https_ota_is_complete_data_received(ota_handle)) {
      LOG_ERR("OTA", "OTA image incomplete on attempt %u/%u", static_cast<unsigned int>(attempt),
              static_cast<unsigned int>(otaMaxAttempts));
      attemptFailed = true;
      attemptError = INTERNAL_UPDATE_ERROR;
      attemptMessage = "Incomplete update data received";
      attemptRetryable = canRetry();
      abortOta();
    }

    if (!attemptFailed) {
      esp_err = esp_https_ota_finish(ota_handle);
      ota_handle = NULL;
      if (esp_err != ESP_OK) {
        LOG_ERR("OTA", "esp_https_ota_finish failed: %s", esp_err_to_name(esp_err));
        attemptFailed = true;
        attemptError = INTERNAL_UPDATE_ERROR;
        attemptMessage = "Failed to finalize update";
        attemptRetryable = false;
      }
    }

    if (!attemptFailed && !selectedChecksum.isEmpty()) {
      size_t imageSize = processedSize;
      if (imageSize == 0) {
        imageSize = totalSize;
      }
      if (imageSize == 0) {
        imageSize = otaSize;
      }
      String checksumError;
      if (updatePartition == nullptr || imageSize == 0 ||
          !verifyPartitionChecksum(updatePartition, selectedChecksum, imageSize, checksumError)) {
        attemptFailed = true;
        attemptError = INTERNAL_UPDATE_ERROR;
        attemptMessage = checksumError.length() > 0 ? checksumError : "Checksum verification failed";
        attemptRetryable = false;
        // esp_https_ota_finish may have set the boot partition; revert to running image.
        const esp_partition_t* runningPartition = esp_ota_get_running_partition();
        if (runningPartition != nullptr) {
          const esp_err_t revertErr = esp_ota_set_boot_partition(runningPartition);
          if (revertErr != ESP_OK) {
            LOG_ERR("OTA", "Failed to restore running boot partition after checksum failure: %s",
                    esp_err_to_name(revertErr));
          }
        }
      }
    }

    if (!attemptFailed) {
      LOG_INF("OTA", "Update completed");

      // Persist installed bundle info
      if (!selectedBundleId.isEmpty()) {
        strncpy(SETTINGS.installedOtaBundle, selectedBundleId.c_str(), sizeof(SETTINGS.installedOtaBundle) - 1);
        SETTINGS.installedOtaBundle[sizeof(SETTINGS.installedOtaBundle) - 1] = '\0';
        strncpy(SETTINGS.installedOtaFeatureFlags, selectedFeatureFlags.c_str(),
                sizeof(SETTINGS.installedOtaFeatureFlags) - 1);
        SETTINGS.installedOtaFeatureFlags[sizeof(SETTINGS.installedOtaFeatureFlags) - 1] = '\0';
        if (!SETTINGS.saveToFile()) {
          LOG_WRN("OTA", "Failed to persist installed bundle metadata to SD card");
        }
      }

      // Write the deferred factory-reset sentinel so boot picks it up on next cold start.
      // The flash itself already succeeded; log a warning if the marker write fails rather than
      // returning an error that would prevent the activity from signalling reboot to the user.
      if (factoryResetOnInstall && !markFactoryResetPending()) {
        LOG_WRN("OTA", "Factory reset marker write failed — reset will NOT occur on next boot");
      }

      return OK;
    }

    finalError = attemptError;
    finalMessage = attemptMessage;
    lastError = attemptMessage;

    if (!attemptRetryable) {
      return attemptError;
    }

    const uint32_t backoffMs = otaRetryBackoffBaseMs << (attempt - 1);
    LOG_WRN("OTA", "Retrying OTA in %lu ms (next attempt %u/%u): %s", static_cast<unsigned long>(backoffMs),
            static_cast<unsigned int>(attempt + 1), static_cast<unsigned int>(otaMaxAttempts), attemptMessage.c_str());
    vTaskDelay(backoffMs / portTICK_PERIOD_MS);
  }

  if (lastError.isEmpty()) {
    lastError = otaCanceled(cancelRequested) ? "Update canceled" : finalMessage;
  }
  if (otaCanceled(cancelRequested)) {
    return INTERNAL_UPDATE_ERROR;
  }
  return finalError;
}
