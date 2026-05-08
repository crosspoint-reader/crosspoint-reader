#include "OtaUpdater.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "HttpClientStream.h"
#include "bootloader_common.h"
#include "esp_flash_partitions.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_wifi.h"

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/" CROSSPOINT_GIT_REPOSITORY "/releases/latest";
constexpr char releaseListUrl[] = "https://api.github.com/repos/" CROSSPOINT_GIT_REPOSITORY "/releases?per_page=1";
constexpr int httpRxBufferSize = 2048;
constexpr int httpTxBufferSize = 512;
constexpr int otaHttpMaxAttempts = 3;
constexpr unsigned long otaInitialRetryDelayMs = 1000;
constexpr size_t releaseMetadataMaxBytes = 128 * 1024;

/*
 * When esp_crt_bundle.h included, it is pointing wrong header file
 * which is something under WifiClientSecure because of our framework based on arduno platform.
 * To manage this obstacle, don't include anything, just extern and it will point correct one.
 */
extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}

esp_err_t http_client_set_header_cb(esp_http_client_handle_t http_client) {
  return esp_http_client_set_header(http_client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
}

struct HttpClientCleaner {
  esp_http_client_handle_t client;
  ~HttpClientCleaner() {
    if (client) {
      esp_http_client_cleanup(client);
    }
  }
};

const char* getReleaseApiUrl() { return SETTINGS.includeBetaUpdates ? releaseListUrl : latestReleaseUrl; }

void delayBeforeRetry(const char* operation, int attempt) {
  const unsigned long delayMs = otaInitialRetryDelayMs << static_cast<unsigned int>(attempt - 1);
  LOG_ERR("OTA", "%s failed on attempt %d/%d, retrying in %lu ms", operation, attempt, otaHttpMaxAttempts, delayMs);
  delay(delayMs);
}

JsonVariantConst selectRelease(const JsonDocument& doc) {
  if (doc.is<JsonArrayConst>()) {
    for (JsonObjectConst release : doc.as<JsonArrayConst>()) {
      if (release["draft"] | false) {
        continue;
      }
      return release;
    }
    return JsonVariantConst();
  }

  if (doc.is<JsonObjectConst>()) {
    return doc.as<JsonObjectConst>();
  }

  return JsonVariantConst();
}
} /* namespace */

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  JsonDocument filter;
  esp_err_t esp_err;
  JsonDocument doc;

  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;
  render = false;

  const char* releaseApiUrl = getReleaseApiUrl();

  esp_http_client_config_t client_config = {
      .url = releaseApiUrl,
      .timeout_ms = 10000,
      /* Default HTTP client buffer size 512 byte only */
      .buffer_size = httpRxBufferSize,
      .buffer_size_tx = httpTxBufferSize,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  if (SETTINGS.includeBetaUpdates) {
    filter[0]["tag_name"] = true;
    filter[0]["draft"] = true;
    filter[0]["assets"][0]["name"] = true;
    filter[0]["assets"][0]["browser_download_url"] = true;
    filter[0]["assets"][0]["size"] = true;
  } else {
    filter["tag_name"] = true;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["browser_download_url"] = true;
    filter["assets"][0]["size"] = true;
  }

  for (int attempt = 1; attempt <= otaHttpMaxAttempts; ++attempt) {
    doc.clear();

    esp_http_client_handle_t client_handle = esp_http_client_init(&client_config);
    if (!client_handle) {
      LOG_ERR("OTA", "HTTP Client Handle Failed");
      return INTERNAL_UPDATE_ERROR;
    }
    HttpClientCleaner clientCleaner = {client_handle};

    esp_err = esp_http_client_set_header(client_handle, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
    if (esp_err != ESP_OK) {
      LOG_ERR("OTA", "esp_http_client_set_header Failed : %s", esp_err_to_name(esp_err));
      return INTERNAL_UPDATE_ERROR;
    }

    esp_err = esp_http_client_set_header(client_handle, "Accept", "application/vnd.github+json");
    if (esp_err != ESP_OK) {
      LOG_ERR("OTA", "esp_http_client_set_header Failed : %s", esp_err_to_name(esp_err));
      return INTERNAL_UPDATE_ERROR;
    }

    esp_err = esp_http_client_open(client_handle, 0);
    if (esp_err != ESP_OK) {
      LOG_ERR("OTA", "esp_http_client_open Failed on attempt %d/%d: %s", attempt, otaHttpMaxAttempts,
              esp_err_to_name(esp_err));
      if (attempt < otaHttpMaxAttempts) {
        delayBeforeRetry("Release metadata connection", attempt);
        continue;
      }
      return HTTP_ERROR;
    }

    const int64_t headerContentLength = esp_http_client_fetch_headers(client_handle);
    if (headerContentLength < 0) {
      LOG_ERR("OTA", "esp_http_client_fetch_headers Failed on attempt %d/%d: %lld", attempt, otaHttpMaxAttempts,
              headerContentLength);
      if (attempt < otaHttpMaxAttempts) {
        delayBeforeRetry("Release metadata headers", attempt);
        continue;
      }
      return HTTP_ERROR;
    }

    const int statusCode = esp_http_client_get_status_code(client_handle);
    if (statusCode != 200) {
      LOG_ERR("OTA", "Release metadata request failed on attempt %d/%d: HTTP %d", attempt, otaHttpMaxAttempts,
              statusCode);
      if (statusCode >= 500 && attempt < otaHttpMaxAttempts) {
        delayBeforeRetry("Release metadata HTTP status", attempt);
        continue;
      }
      return HTTP_ERROR;
    }

    const bool chunked = esp_http_client_is_chunked_response(client_handle);
    const int64_t contentLength = chunked ? -1 : esp_http_client_get_content_length(client_handle);
    LOG_DBG("OTA", "Release metadata headers: content_length=%lld chunked=%s heap=%u largest=%u", contentLength,
            chunked ? "yes" : "no", heap_caps_get_free_size(MALLOC_CAP_8BIT),
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    if (contentLength > static_cast<int64_t>(releaseMetadataMaxBytes)) {
      LOG_ERR("OTA", "Release metadata too large: %lld bytes", contentLength);
      return METADATA_TOO_LARGE_ERROR;
    }

    HttpClientStream responseStream(client_handle, contentLength, releaseMetadataMaxBytes);
    const DeserializationError error = deserializeJson(doc, responseStream, DeserializationOption::Filter(filter));
    if (error) {
      if (responseStream.isLimitExceeded() || error == DeserializationError::NoMemory) {
        LOG_ERR("OTA", "Release metadata too large after %zu bytes: %s", responseStream.bytesReadCount(),
                error.c_str());
        return METADATA_TOO_LARGE_ERROR;
      }
      if (responseStream.hasError()) {
        LOG_ERR("OTA", "HTTP stream read failed on attempt %d/%d after %zu bytes: %d", attempt, otaHttpMaxAttempts,
                responseStream.bytesReadCount(), responseStream.lastError());
        if (attempt < otaHttpMaxAttempts) {
          delayBeforeRetry("Release metadata stream", attempt);
          continue;
        }
        return HTTP_ERROR;
      }
      LOG_ERR("OTA", "JSON parse failed after %zu bytes: %s", responseStream.bytesReadCount(), error.c_str());
      return JSON_PARSE_ERROR;
    }

    break;
  }

  const JsonVariantConst release = selectRelease(doc);
  if (release.isNull()) {
    LOG_ERR("OTA", "No release found in response");
    return JSON_PARSE_ERROR;
  }

  if (!release["tag_name"].is<std::string>()) {
    LOG_ERR("OTA", "No tag_name found");
    return JSON_PARSE_ERROR;
  }

  if (!release["assets"].is<JsonArrayConst>()) {
    LOG_ERR("OTA", "No assets found");
    return JSON_PARSE_ERROR;
  }

  latestVersion = release["tag_name"].as<std::string>();

  for (JsonObjectConst asset : release["assets"].as<JsonArrayConst>()) {
    if (asset["name"] == "firmware.bin") {
      otaUrl = asset["browser_download_url"].as<std::string>();
      otaSize = asset["size"].as<size_t>();
      totalSize = otaSize;
      updateAvailable = true;
      break;
    }
  }

  if (!updateAvailable) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

  LOG_DBG("OTA", "Found %s update: %s", SETTINGS.includeBetaUpdates ? "beta" : "stable", latestVersion.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor = 0, currentMinor = 0, currentPatch = 0, currentBetaRelease = 0, currentBetaBuild = 0;
  int latestMajor = 0, latestMinor = 0, latestPatch = 0, latestBetaRelease = 0, latestBetaBuild = 0;

  const auto currentVersion = CROSSPOINT_VERSION;
  const bool currentIsBeta = strstr(currentVersion, "-rc.") != nullptr;
  const bool latestIsBeta = latestVersion.find("-rc.") != std::string::npos;

  // Semantic version check with optional RC suffix. `sscanf()` will stop when
  // it reaches part of the input string that doesn't match the format, so this
  // format string works for versions like "1.31", "1.34.2", "1.35.0-rc.1", and
  // "1.36.0-rc.2.5".
  // This does not handle versions using the old "rc.<hash>" format, but
  // considering that people will need to manually install this release or later
  // to get this functionality anyway that should be fine.
  sscanf(latestVersion.c_str(), "%d.%d.%d-rc.%d.%d", &latestMajor, &latestMinor, &latestPatch, &latestBetaRelease,
         &latestBetaBuild);
  sscanf(currentVersion, "%d.%d.%d-rc.%d.%d", &currentMajor, &currentMinor, &currentPatch, &currentBetaRelease,
         &currentBetaBuild);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  /*
   * If we reach here, the stable version segments are equal. A stable release
   * is newer than an RC with the same version.
   */
  if (!latestIsBeta && currentIsBeta) {
    return true;
  }

  if (latestIsBeta && !currentIsBeta) {
    return false;
  }

  /*
   * If both versions are RCs, compare their RC release and build numbers.
   */
  if (latestIsBeta && currentIsBeta) {
    if (latestBetaRelease != currentBetaRelease) {
      return latestBetaRelease > currentBetaRelease;
    }
    if (latestBetaBuild != currentBetaBuild) {
      return latestBetaBuild > currentBetaBuild;
    }
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

void OtaUpdater::cleanupUpdate() {
  if (otaHandle) {
    const esp_err_t err = esp_https_ota_finish(otaHandle);
    if (err != ESP_OK) {
      LOG_ERR("OTA", "esp_https_ota_finish on cleanup: %s", esp_err_to_name(err));
    }
    otaHandle = nullptr;
  }
  cancelRequested = false;
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

void OtaUpdater::cancelUpdate() {
  if (otaHandle) {
    cleanupUpdate();
  } else {
    cancelRequested = true;
  }
}

OtaUpdater::OtaUpdaterError OtaUpdater::beginInstallUpdate() {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  cleanupUpdate();
  render = false;
  cancelRequested = false;

  esp_http_client_config_t client_config = {
      .url = otaUrl.c_str(),
      .timeout_ms = 10000,
      .max_redirection_count = 5,
      .buffer_size = 8192,
      .buffer_size_tx = 8192,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  esp_https_ota_config_t ota_config = {
      .http_config = &client_config,
      .http_client_init_cb = http_client_set_header_cb,
  };

  for (int attempt = 1; attempt <= otaHttpMaxAttempts; ++attempt) {
    /* For better timing and connectivity, we disable power saving for WiFi */
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_err_t esp_err = esp_https_ota_begin(&ota_config, &otaHandle);
    if (esp_err == ESP_OK) {
      return UPDATE_IN_PROGRESS;
    }

    LOG_ERR("OTA", "HTTP OTA Begin Failed on attempt %d/%d: %s", attempt, otaHttpMaxAttempts, esp_err_to_name(esp_err));
    cleanupUpdate();
    if (attempt < otaHttpMaxAttempts) {
      delayBeforeRetry("Firmware OTA connection", attempt);
    }
  }

  return INTERNAL_UPDATE_ERROR;
}

/* Writes the otadata entry to boot from the most recently flashed OTA partition,
 * bypassing esp_ota_set_boot_partition()'s image_validate() call.
 * Used when esp_https_ota_finish() returns ESP_ERR_OTA_VALIDATE_FAILED on
 * unsigned Arduino builds (boot_comm efuse revision check false-positive). */
int OtaUpdater::forceSetOtaBootPartition() {
  const esp_partition_t* newPartition = esp_ota_get_next_update_partition(nullptr);
  if (newPartition == nullptr) {
    LOG_ERR("OTA", "force boot partition: next update partition not found");
    return ESP_ERR_NOT_FOUND;
  }
  LOG_INF("OTA", "force boot next partition=%s subtype=0x%x offset=0x%lx size=0x%lx", newPartition->label,
          newPartition->subtype, static_cast<unsigned long>(newPartition->address),
          static_cast<unsigned long>(newPartition->size));

  const esp_partition_t* otaDataPartition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (otaDataPartition == nullptr) {
    LOG_ERR("OTA", "force boot partition: otadata partition not found");
    return ESP_ERR_NOT_FOUND;
  }
  esp_ota_select_entry_t otadata[2];
  esp_err_t err = esp_partition_read(otaDataPartition, 0, &otadata[0], sizeof(esp_ota_select_entry_t));
  if (err != ESP_OK) {
    LOG_ERR("OTA", "force boot: read otadata[0] failed: %s", esp_err_to_name(err));
    return err;
  }
  err = esp_partition_read(otaDataPartition, otaDataPartition->erase_size, &otadata[1], sizeof(esp_ota_select_entry_t));
  if (err != ESP_OK) {
    LOG_ERR("OTA", "force boot: read otadata[1] failed: %s", esp_err_to_name(err));
    return err;
  }

  int activeSlot = bootloader_common_get_active_otadata(otadata);
  int nextSlot = (activeSlot == -1) ? 0 : (~activeSlot & 1);

  uint8_t otaAppCount = 0;
  while (esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                  static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + otaAppCount),
                                  nullptr) != nullptr) {
    otaAppCount++;
  }
  if (otaAppCount == 0) {
    LOG_ERR("OTA", "force boot select: no OTA app partitions found");
    return ESP_ERR_NOT_FOUND;
  }

  const uint8_t subTypeId = newPartition->subtype & 0x0F;
  uint32_t newSeq;
  if (activeSlot == -1) {
    newSeq = subTypeId + 1;
  } else {
    uint32_t currentSeq = otadata[activeSlot].ota_seq;
    newSeq = currentSeq;
    // ESP-IDF's bootloader maps ota_seq to an OTA app slot with
    // (ota_seq - 1) % ota_app_count. Match that mapping here so the forced
    // otadata entry selects the partition that esp_https_ota just wrote.
    while ((newSeq - 1) % otaAppCount != static_cast<uint32_t>(subTypeId)) {
      newSeq++;
    }
    if (newSeq == currentSeq) newSeq += otaAppCount;
  }

  otadata[nextSlot].ota_seq = newSeq;
  otadata[nextSlot].ota_state = ESP_OTA_IMG_VALID;
  otadata[nextSlot].crc = bootloader_common_ota_select_crc(&otadata[nextSlot]);

  err = esp_partition_erase_range(otaDataPartition, otaDataPartition->erase_size * static_cast<uint32_t>(nextSlot),
                                  otaDataPartition->erase_size);
  if (err != ESP_OK) {
    LOG_ERR("OTA", "force boot: erase otadata[%d] failed: %s", nextSlot, esp_err_to_name(err));
    return err;
  }

  err = esp_partition_write(otaDataPartition, otaDataPartition->erase_size * static_cast<uint32_t>(nextSlot),
                            &otadata[nextSlot], sizeof(esp_ota_select_entry_t));
  if (err != ESP_OK) {
    LOG_ERR("OTA", "force boot: write otadata[%d] failed: %s", nextSlot, esp_err_to_name(err));
  }
  return err;
}

OtaUpdater::OtaUpdaterError OtaUpdater::performInstallUpdateStep() {
  if (cancelRequested) {
    cleanupUpdate();
    return UPDATE_CANCELLED;
  }

  if (!otaHandle) {
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err_t esp_err = esp_https_ota_perform(otaHandle);
  processedSize = esp_https_ota_get_image_len_read(otaHandle);
  render = true;

  if (esp_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    return UPDATE_IN_PROGRESS;
  }

  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s", esp_err_to_name(esp_err));
    cleanupUpdate();
    return HTTP_ERROR;
  }

  if (!esp_https_ota_is_complete_data_received(otaHandle)) {
    LOG_ERR("OTA", "esp_https_ota_is_complete_data_received Failed");
    cleanupUpdate();
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err_t finish_err = esp_https_ota_finish(otaHandle);
  otaHandle = nullptr;
  if (finish_err == ESP_ERR_OTA_VALIDATE_FAILED) {
    /* Arduino unsigned builds fail boot_comm validation even though the image
     * is fully written. Force the boot partition to the new OTA slot by writing
     * the otadata entry directly, bypassing image_validate(). */
    LOG_INF("OTA", "Validation failed (expected for unsigned Arduino builds) - forcing boot partition");
    finish_err = forceSetOtaBootPartition();
    if (finish_err != ESP_OK) {
      LOG_ERR("OTA", "forceSetOtaBootPartition failed: %s", esp_err_to_name(finish_err));
      cleanupUpdate();
      return VALIDATE_FAILED;
    }
  } else if (finish_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_finish Failed: %s", esp_err_to_name(finish_err));
    cleanupUpdate();
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
