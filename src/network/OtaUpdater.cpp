#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first. Pin this order; clang-format would otherwise sort
// the local header last and break the build.
#include "HttpDownloader.h"
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
// clang-format on

#include <algorithm>
#include <cstring>
#include <string>

#include "FirmwareBoardTag.h"
#include "FirmwareFlasher.h"
#include "FirmwareImageIdentity.h"

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/latest";
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  // Stream the ~32KB release JSON straight into the parser as it arrives.
  // Buffering the whole body in a std::string would add a growing allocation
  // on top of the TLS session's heap during the fetch; with -fno-exceptions an
  // OOM there aborts. fetchUrl handles the verified-https GET, redirects, and
  // User-Agent (see HttpDownloader).
  ReleaseJsonParser releaseParser;
  // Each board updates from its own release asset: plain firmware.bin for the
  // C3 X4/X3 binary (pre-existing releases), firmware-<board>.bin otherwise.
  const bool isX4 = board_tag::boardNameLen() == 2 && memcmp(board_tag::boardName(), "x4", 2) == 0;
  char assetName[48] = "firmware.bin";
  if (!isX4) {
    snprintf(assetName, sizeof(assetName), "firmware-%.*s.bin", static_cast<int>(board_tag::boardNameLen()),
             board_tag::boardName());
  }
  releaseParser.setFirmwareAssetName(assetName);
  const bool ok = HttpDownloader::fetchUrl(latestReleaseUrl, [&releaseParser](const uint8_t* data, size_t len) {
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_INF("OTA", "No %s asset in latest release", assetName);
    return NO_UPDATE;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
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

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  // esp_https_ota is hardwired to esp-tls/mbedTLS, whose precompiled build on this
  // package can't negotiate TLS 1.3 (see SecureClient.h). Drive the OTA partition
  // ourselves and stream the firmware through HttpDownloader, which runs over
  // wolfSSL when FREEINK_NET_WOLFSSL is set, reusing its redirect handling for the
  // GitHub -> CDN hop.
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (!updatePartition) {
    LOG_ERR("OTA", "No OTA partition available");
    return INTERNAL_UPDATE_ERROR;
  }

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  processedSize = 0;
  int lastReportedPct = -1;
  bool flashOk = true;
  // esp_ota_begin() erases the whole destination partition, so it is deferred
  // until the image header has arrived and been accepted. Erasing first would
  // destroy the fallback slot on behalf of an image we then refuse.
  esp_ota_handle_t otaHandle = 0;
  bool otaBegun = false;
  bool beginFailed = false;
  // The image streams in chunks; only the first bytes carry the header. Buffer
  // a whole esp_image_header_t, then flush it into the partition once the
  // compatibility decision has passed.
  uint8_t header[firmware_identity::IMAGE_HEADER_SIZE];
  size_t headerLen = 0;
  bool wrongChip = false;
  // All S3 boards share a chip_id, so also scan the stream for the embedded
  // board tag (FirmwareBoardTag.h). An untagged image passes; a tag naming a
  // different board aborts the download. Unlike chip_id the tag lives in the
  // image body and cannot be known up front, so a wrong-board image may
  // partially land in the inactive OTA slot -- esp_ota_abort() below means it
  // never becomes the boot target.
  board_tag::Scanner tagScanner;
  const bool fetchOk = HttpDownloader::fetchUrl(otaUrl, [&](const uint8_t* data, const size_t len) {
    // Every byte must reach the scanner in stream order, including the header
    // bytes buffered below, or a tag straddling that boundary is missed.
    tagScanner.feed(data, len);
    if (tagScanner.mismatch()) {
      LOG_ERR("OTA", "wrong board: image=%s device=%.*s", tagScanner.foundName(),
              static_cast<int>(board_tag::boardNameLen()), board_tag::boardName());
      return false;  // abort the transfer
    }

    size_t consumed = 0;
    if (!otaBegun) {
      const size_t take = std::min(len, sizeof(header) - headerLen);
      std::memcpy(header + headerLen, data, take);
      headerLen += take;
      consumed = take;
      if (headerLen < sizeof(header)) {
        processedSize += len;
        return true;  // still collecting the header; nothing erased yet
      }

      uint16_t imageChip = 0;
      if (!firmware_identity::readImageChipId(header, headerLen, imageChip)) {
        // A partial header yields no chip id at all rather than a zero, which
        // would read as a valid ESP32 id.
        LOG_ERR("OTA", "image header too short for chip id");
        beginFailed = true;
        return false;  // abort the transfer
      }
      const uint16_t deviceChip = firmware_flash::deviceChipId();
      const auto verdict = firmware_identity::compareChipId(imageChip, deviceChip);
      if (verdict == firmware_identity::ChipVerdict::Mismatch) {
        LOG_ERR("OTA", "wrong chip: image=0x%04X device=0x%04X", imageChip, deviceChip);
        wrongChip = true;
        return false;  // abort the transfer, partition still intact
      }
      if (verdict == firmware_identity::ChipVerdict::UnknownDeviceIdentity) {
        // Fail open: a device whose identity source is broken must still be
        // updatable. The bootloader re-checks chip_id on every boot.
        LOG_ERR("OTA", "device chip id unknown; chip guard skipped (image=0x%04X)", imageChip);
      }

      const esp_err_t beginErr = esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &otaHandle);
      if (beginErr != ESP_OK) {
        LOG_ERR("OTA", "esp_ota_begin failed: %s", esp_err_to_name(beginErr));
        beginFailed = true;
        return false;  // abort the transfer
      }
      otaBegun = true;

      if (esp_ota_write(otaHandle, header, headerLen) != ESP_OK) {
        flashOk = false;
        return false;  // abort the transfer
      }
    }

    const size_t bodyLen = len - consumed;
    if (bodyLen > 0 && esp_ota_write(otaHandle, data + consumed, bodyLen) != ESP_OK) {
      flashOk = false;
      return false;  // abort the transfer
    }
    processedSize += len;
    // Fire the callback only on whole-percent change. Per-chunk updates wake the
    // render task, whose framebuffer work contends with TLS on the internal arena,
    // and e-ink can't repaint faster than a percent tick anyway.
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
    return true;
  });

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  // esp_ota_abort() is only valid on a handle esp_ota_begin() actually opened;
  // a chip rejection now happens before that, leaving nothing to abort.
  if (wrongChip || tagScanner.mismatch()) {
    LOG_ERR("OTA", "Firmware install aborted: wrong device");
    if (otaBegun) esp_ota_abort(otaHandle);
    return WRONG_DEVICE_ERROR;
  }

  if (beginFailed) {
    if (otaBegun) esp_ota_abort(otaHandle);
    return INTERNAL_UPDATE_ERROR;
  }

  if (!fetchOk || !flashOk) {
    LOG_ERR("OTA", "Firmware install failed (%s)", flashOk ? "download" : "flash write");
    if (otaBegun) esp_ota_abort(otaHandle);
    return flashOk ? HTTP_ERROR : INTERNAL_UPDATE_ERROR;
  }

  if (!otaBegun) {
    LOG_ERR("OTA", "transfer ended before a complete image header (%u bytes)", static_cast<unsigned>(headerLen));
    return HTTP_ERROR;
  }

  esp_err_t esp_err = esp_ota_end(otaHandle);  // verifies the written image
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
