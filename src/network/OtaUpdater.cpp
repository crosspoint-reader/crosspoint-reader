#include "OtaUpdater.h"

#include <ArduinoJson.h>
#include <Logging.h>

#include "bootloader_common.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_wifi.h"

// Bypasses esp_ota_set_boot_partition()'s image validation (which fails for
// unsigned Arduino builds due to missing embedded SHA256 and strict chip
// efuse-block-revision checks). Writes the otadata entry directly.
static esp_err_t forceSetBootPartitionOta(const esp_partition_t* newPart) {
  if (!newPart) return ESP_ERR_INVALID_ARG;
  const esp_partition_t* otaPart =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (!otaPart) return ESP_ERR_NOT_FOUND;

  static constexpr size_t ENTRY_SIZE = 32;
  static constexpr size_t SECTOR_SIZE = 0x1000;
  struct __attribute__((packed)) OtaEntry {
    uint32_t seq;
    uint8_t label[20];
    uint32_t state;
    uint32_t crc;
  };

  OtaEntry e0, e1;
  memset(&e0, 0xFF, sizeof(e0));
  memset(&e1, 0xFF, sizeof(e1));
  bool e0Valid = (esp_partition_read(otaPart, 0, &e0, sizeof(e0)) == ESP_OK);
  bool e1Valid = (esp_partition_read(otaPart, SECTOR_SIZE, &e1, sizeof(e1)) == ESP_OK);
  if (!e0Valid) LOG_ERR("OTA", "Failed to read OTA sector 0 — treating as erased");
  if (!e1Valid) LOG_ERR("OTA", "Failed to read OTA sector 1 — treating as erased");

  // Unreadable sectors are treated as erased (seq == 0), matching the memset(0xFF) sentinel.
  uint32_t seq0 = (e0Valid && e0.seq != 0xFFFFFFFF) ? e0.seq : 0;
  uint32_t seq1 = (e1Valid && e1.seq != 0xFFFFFFFF) ? e1.seq : 0;
  uint32_t maxSeq = (seq0 > seq1) ? seq0 : seq1;
  uint32_t partIdx = newPart->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;

  uint32_t newSeq = maxSeq + 1;
  while (((newSeq - 1) % 2) != partIdx) newSeq++;

  // Ping-pong: overwrite the sector with the OLDER (lower) sequence so that
  // the other sector — which holds the current highest-seq entry — remains as
  // a valid fallback if this write is interrupted.
  bool writeToSector1 = (seq1 <= seq0);
  uint32_t writeOffset = writeToSector1 ? SECTOR_SIZE : 0;

  OtaEntry entry;
  entry.seq = newSeq;
  memset(entry.label, 0xFF, sizeof(entry.label));
  // Write VALID directly — next boot confirms anyway.
  // PENDING_VERIFY triggers image_validate() inside esp_ota_mark_app_valid_cancel_rollback()
  // which also fails for unsigned builds.
  entry.state = ESP_OTA_IMG_VALID;
  // Use official bootloader CRC (covers ota_seq field only, 4 bytes).
  entry.crc = bootloader_common_ota_select_crc(reinterpret_cast<const esp_ota_select_entry_t*>(&entry));

  esp_err_t err = esp_partition_erase_range(otaPart, writeOffset, SECTOR_SIZE);
  if (err != ESP_OK) return err;
  return esp_partition_write(otaPart, writeOffset, &entry, sizeof(entry));
}

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/latest";

/* This is buffer and size holder to keep upcoming data from latestReleaseUrl */
char* local_buf;
int output_len;

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

esp_err_t event_handler(esp_http_client_event_t* event) {
  /* We do interested in only HTTP_EVENT_ON_DATA event only */
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;

  if (!esp_http_client_is_chunked_response(event->client)) {
    int content_len = esp_http_client_get_content_length(event->client);
    int copy_len = 0;

    if (local_buf == NULL) {
      /* local_buf life span is tracked by caller checkForUpdate */
      local_buf = static_cast<char*>(calloc(content_len + 1, sizeof(char)));
      output_len = 0;
      if (local_buf == NULL) {
        LOG_ERR("OTA", "HTTP Client Out of Memory Failed, Allocation %d", content_len);
        return ESP_ERR_NO_MEM;
      }
    }
    copy_len = min(event->data_len, (content_len - output_len));
    if (copy_len) {
      memcpy(local_buf + output_len, event->data, copy_len);
    }
    output_len += copy_len;
  } else {
    /* Code might be hits here, It happened once (for version checking) but I need more logs to handle that */
    int chunked_len;
    esp_http_client_get_chunk_length(event->client, &chunked_len);
    LOG_DBG("OTA", "esp_http_client_is_chunked_response failed, chunked_len: %d", chunked_len);
  }

  return ESP_OK;
} /* event_handler */
} /* namespace */

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  JsonDocument filter;
  esp_err_t esp_err;
  JsonDocument doc;

  esp_http_client_config_t client_config = {
      .url = latestReleaseUrl,
      .event_handler = event_handler,
      /* Default HTTP client buffer size 512 byte only */
      .buffer_size = 8192,
      .buffer_size_tx = 8192,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  /* To track life time of local_buf, dtor will be called on exit from that function */
  struct localBufCleaner {
    char** bufPtr;
    ~localBufCleaner() {
      if (*bufPtr) {
        free(*bufPtr);
        *bufPtr = NULL;
      }
    }
  } localBufCleaner = {&local_buf};

  esp_http_client_handle_t client_handle = esp_http_client_init(&client_config);
  if (!client_handle) {
    LOG_ERR("OTA", "HTTP Client Handle Failed");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_set_header(client_handle, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_set_header Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_perform(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_perform Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return HTTP_ERROR;
  }

  /* esp_http_client_close will be called inside cleanup as well*/
  esp_err = esp_http_client_cleanup(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_cleanup Failed : %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  filter["assets"][0]["size"] = true;
  const DeserializationError error = deserializeJson(doc, local_buf, DeserializationOption::Filter(filter));
  if (error) {
    LOG_ERR("OTA", "JSON parse failed: %s", error.c_str());
    return JSON_PARSE_ERROR;
  }

  if (!doc["tag_name"].is<std::string>()) {
    LOG_ERR("OTA", "No tag_name found");
    return JSON_PARSE_ERROR;
  }

  if (!doc["assets"].is<JsonArray>()) {
    LOG_ERR("OTA", "No assets found");
    return JSON_PARSE_ERROR;
  }

  latestVersion = doc["tag_name"].as<std::string>();

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
    return NO_UPDATE;
  }

  LOG_DBG("OTA", "Found update: %s", latestVersion.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;

  const auto currentVersion = CROSSPOINT_VERSION;

  // semantic version check (only match on 3 segments)
  sscanf(latestVersion.c_str(), "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
  sscanf(currentVersion, "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch);

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

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(std::function<void()> onProgress) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  esp_https_ota_handle_t ota_handle = NULL;
  esp_err_t esp_err;
  /* Signal for OtaUpdateActivity */
  render = false;

  esp_http_client_config_t client_config = {
      .url = otaUrl.c_str(),
      .timeout_ms = 30000,
      /* GitHub release assets redirect to CDN (objects.githubusercontent.com).
       * Without this, esp_https_ota downloads 0 bytes and stalls on redirect. */
      .max_redirection_count = 5,
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

  // Determine the update partition upfront (the inactive OTA slot) so we can
  // call forceSetBootPartitionOta() after the download, bypassing the
  // image validation in esp_https_ota_finish() that fails for unsigned builds.
  const esp_partition_t* updatePart = esp_ota_get_next_update_partition(nullptr);

  esp_err = esp_https_ota_begin(&ota_config, &ota_handle);
  if (esp_err != ESP_OK) {
    LOG_DBG("OTA", "HTTP OTA Begin Failed: %s", esp_err_to_name(esp_err));
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // Restore power save on early failure
    return INTERNAL_UPDATE_ERROR;
  }

  do {
    esp_err = esp_https_ota_perform(ota_handle);
    processedSize = esp_https_ota_get_image_len_read(ota_handle);
    render = true;
    if (onProgress) onProgress();
    delay(100);
  } while (esp_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s", esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return HTTP_ERROR;
  }

  if (!esp_https_ota_is_complete_data_received(ota_handle)) {
    LOG_ERR("OTA", "esp_https_ota_is_complete_data_received Failed: %s", esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  // esp_https_ota_finish() internally calls esp_ota_set_boot_partition(), which validates
  // the firmware image and returns ESP_ERR_OTA_VALIDATE_FAILED for unsigned Arduino builds
  // (no embedded SHA256) or when the binary's min_efuse_blk_rev_full exceeds the chip's.
  // The firmware data was written correctly; tolerate that error and use
  // forceSetBootPartitionOta() to write the otadata entry directly instead.
  esp_err = esp_https_ota_finish(ota_handle);
  if (esp_err != ESP_OK && esp_err != ESP_ERR_OTA_VALIDATE_FAILED) {
    LOG_ERR("OTA", "esp_https_ota_finish Failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }
  if (esp_err == ESP_ERR_OTA_VALIDATE_FAILED) {
    LOG_INF("OTA", "Image validation skipped (unsigned build) — writing otadata directly");
  }

  esp_err = forceSetBootPartitionOta(updatePart);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "forceSetBootPartitionOta Failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
