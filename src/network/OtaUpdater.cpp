#include "OtaUpdater.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_flash.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <spi_flash_mmap.h>

#include <algorithm>
#include <memory>

#include "FirmwareFlasher.h"
#include "esp_http_client.h"
#include "esp_wifi.h"

namespace {

constexpr char latestReleaseUrl[] = "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/latest";

/* This is buffer and size holder to keep upcoming data from latestReleaseUrl */
char* local_buf;
int output_len;
int buf_cap;

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

/*
 * Initial buffer size used for chunked responses (no Content-Length header).
 * Grows geometrically via realloc if the response exceeds this.
 */
constexpr int kChunkedInitialBuf = 16384;

esp_err_t event_handler(esp_http_client_event_t* event) {
  /* We do interested in only HTTP_EVENT_ON_DATA event only */
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;

  const bool chunked = esp_http_client_is_chunked_response(event->client);
  int content_len = esp_http_client_get_content_length(event->client);

  /* First data event: allocate the backing buffer. */
  if (local_buf == NULL) {
    const int initial = (chunked || content_len <= 0) ? kChunkedInitialBuf : (content_len + 1);
    local_buf = static_cast<char*>(calloc(initial, sizeof(char)));
    output_len = 0;
    buf_cap = initial;
    if (local_buf == NULL) {
      LOG_ERR("OTA", "HTTP Client Out of Memory Failed, Allocation %d", initial);
      return ESP_ERR_NO_MEM;
    }
  }

  /* Grow buffer for chunked/unknown-length responses. */
  if (output_len + event->data_len + 1 > buf_cap) {
    int new_cap = buf_cap * 2;
    while (new_cap < output_len + event->data_len + 1) new_cap *= 2;
    char* new_buf = static_cast<char*>(realloc(local_buf, new_cap));
    if (new_buf == NULL) {
      LOG_ERR("OTA", "HTTP Client realloc Failed, target %d", new_cap);
      return ESP_ERR_NO_MEM;
    }
    local_buf = new_buf;
    memset(local_buf + buf_cap, 0, new_cap - buf_cap);
    buf_cap = new_cap;
  }

  int copy_len = event->data_len;
  if (!chunked && content_len > 0) {
    copy_len = min(copy_len, content_len - output_len);
  }
  if (copy_len > 0) {
    memcpy(local_buf + output_len, event->data, copy_len);
    output_len += copy_len;
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
      /* 15s covers WiFi-warmup TLS handshake jitter right after WifiSelection. */
      .timeout_ms = 15000,
      .event_handler = event_handler,
      /* Default HTTP client buffer size 512 byte only */
      .buffer_size = 8192,
      .buffer_size_tx = 8192,
      .skip_cert_common_name_check = true,
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

  /* Reset per-call state: prior aborted attempt may have left stale values. */
  local_buf = NULL;
  output_len = 0;
  buf_cap = 0;

  esp_err = ESP_FAIL;
  for (int attempt = 0; attempt < 2; ++attempt) {
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
    esp_http_client_cleanup(client_handle);
    if (esp_err == ESP_OK && output_len > 0) break;

    LOG_ERR("OTA", "perform attempt %d failed: %s (len=%d)", attempt, esp_err_to_name(esp_err), output_len);
    /* Drop any partial buffer before retrying. */
    if (local_buf) {
      free(local_buf);
      local_buf = NULL;
    }
    output_len = 0;
    buf_cap = 0;
    delay(500);
  }
  if (esp_err != ESP_OK || output_len == 0) {
    return HTTP_ERROR;
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

namespace {
constexpr const char* kOtaSdPath = "/.crosspoint/ota_firmware.bin";

// Stash the activity-side progress callback so the firmware-flasher's free
// progress callback can fan back into it (we need a void* ctx hop).
struct FlashCtx {
  OtaUpdater* updater;
  OtaUpdater::ProgressCallback onProgress;
  void* userCtx;
};

// Per-call download state shared with the event handler.
struct DownloadCtx {
  OtaUpdater* updater;
  HalFile* sdFile;
  size_t written;
  bool writeFailed;
  OtaUpdater::ProgressCallback onProgress;
  void* userCtx;
};

esp_err_t download_event_handler(esp_http_client_event_t* evt) {
  auto* dctx = static_cast<DownloadCtx*>(evt->user_data);
  switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
      // capture Content-Length when the server provides it
      if (evt->header_key && evt->header_value && strcasecmp(evt->header_key, "Content-Length") == 0) {
        const int len = atoi(evt->header_value);
        if (len > 0) dctx->updater->setExpectedSize(static_cast<size_t>(len));
      }
      break;
    case HTTP_EVENT_ON_DATA:
      if (dctx->writeFailed) return ESP_OK;
      if (evt->data_len > 0 && dctx->sdFile && *dctx->sdFile) {
        const size_t want = static_cast<size_t>(evt->data_len);
        const size_t wrote = dctx->sdFile->write(static_cast<const uint8_t*>(evt->data), want);
        if (wrote != want) {
          LOG_ERR("OTA", "SD write short @%u (got=%u want=%u)", static_cast<unsigned>(dctx->written),
                  static_cast<unsigned>(wrote), static_cast<unsigned>(want));
          dctx->writeFailed = true;
          dctx->updater->setLastError("sd_write");
          return ESP_FAIL;
        }
        dctx->written += want;
        dctx->updater->setProcessed(dctx->written);
        if (dctx->onProgress) dctx->onProgress(dctx->userCtx);
      }
      break;
    default:
      break;
  }
  return ESP_OK;
}
}  // namespace

void OtaUpdater::setLastError(const std::string& err) { lastError = err; }
void OtaUpdater::setExpectedSize(size_t s) {
  totalSize = s;
  render = true;
}
void OtaUpdater::setProcessed(size_t s) {
  processedSize = s;
  render = true;
}

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  lastError.clear();
  if (!isUpdateNewer()) {
    lastError = "not_newer";
    return UPDATE_OLDER_ERROR;
  }

  // Two-phase: (1) download the patched firmware.bin to SD via
  // esp_http_client_perform (same proven pattern as checkForUpdate uses) +
  // event handler that streams chunks straight to a file on the SD card,
  // (2) flash that file using firmware_flash::flashFromSdPath. Skipping
  // esp_https_ota_* avoids the running ESP-IDF's bogus esp_image_verify
  // efuse-blk-rev rejection on X4 silicon. The cached SD file also lets
  // the user retry the SD update flow if anything dies mid-flash.
  phase = Phase::Downloading;
  totalSize = otaSize;  // GitHub release asset size — pre-populated from checkForUpdate
  processedSize = 0;
  render = true;
  if (onProgress) onProgress(ctx);

  Storage.mkdir("/.crosspoint", true);

  HalFile sdFile;
  if (!Storage.openFileForWrite("OTA", kOtaSdPath, sdFile) || !sdFile) {
    LOG_ERR("OTA", "open SD cache for write failed: %s", kOtaSdPath);
    lastError = "sd_open";
    return INTERNAL_UPDATE_ERROR;
  }

  DownloadCtx dctx{this, &sdFile, 0, false, onProgress, ctx};

  esp_http_client_config_t client_config = {
      .url = otaUrl.c_str(),
      .timeout_ms = 30000,
      .event_handler = download_event_handler,
      .buffer_size = 8192,
      .buffer_size_tx = 8192,
      .user_data = &dctx,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  esp_http_client_handle_t client = esp_http_client_init(&client_config);
  if (!client) {
    LOG_ERR("OTA", "esp_http_client_init failed");
    sdFile.close();
    lastError = "http_init";
    return INTERNAL_UPDATE_ERROR;
  }
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  esp_wifi_set_ps(WIFI_PS_NONE);

  esp_err_t err = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  sdFile.close();

  if (err != ESP_OK) {
    LOG_ERR("OTA", "http perform failed: %s (status=%d)", esp_err_to_name(err), status);
    char buf[48];
    snprintf(buf, sizeof(buf), "http_perform:%s", esp_err_to_name(err));
    lastError = buf;
    return HTTP_ERROR;
  }
  if (status / 100 != 2) {
    LOG_ERR("OTA", "http status %d", status);
    char buf[24];
    snprintf(buf, sizeof(buf), "http_status:%d", status);
    lastError = buf;
    return HTTP_ERROR;
  }
  if (dctx.writeFailed) {
    return INTERNAL_UPDATE_ERROR;  // lastError already set
  }
  if (dctx.written == 0) {
    LOG_ERR("OTA", "no body bytes received");
    lastError = "empty_body";
    return HTTP_ERROR;
  }
  LOG_INF("OTA", "download complete: %u bytes -> %s", static_cast<unsigned>(dctx.written), kOtaSdPath);

  // Phase 2: flash from SD using the shared firmware flasher.
  phase = Phase::Flashing;
  totalSize = dctx.written;
  processedSize = 0;
  render = true;
  if (onProgress) onProgress(ctx);

  FlashCtx flashCtx{this, onProgress, ctx};
  auto progressCb = +[](size_t written, size_t total, void* fctx) {
    auto* fc = static_cast<FlashCtx*>(fctx);
    fc->updater->setProcessed(written);
    fc->updater->setExpectedSize(total);
    if (fc->onProgress) fc->onProgress(fc->userCtx);
  };

  const auto fr = firmware_flash::flashFromSdPath(kOtaSdPath, progressCb, &flashCtx);
  if (fr != firmware_flash::Result::OK) {
    LOG_ERR("OTA", "flash failed: %s", firmware_flash::resultName(fr));
    char buf[32];
    snprintf(buf, sizeof(buf), "flash:%s", firmware_flash::resultName(fr));
    lastError = buf;
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
