#include "FirmwareUpdateUtil.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <SpiBusMutex.h>

#include "esp_ota_ops.h"
#include "fontIds.h"

bool FirmwareUpdateUtil::checkForLocalUpdate() {
  SpiBusMutex::Guard guard;
  return Storage.exists(kFirmwareBinPath);
}

bool FirmwareUpdateUtil::performLocalUpdate(GfxRenderer& renderer) {
  LOG_INF("FWUPD", "Starting local firmware update from %s", kFirmwareBinPath);

  FsFile firmwareFile;
  size_t firmwareSize = 0;
  {
    SpiBusMutex::Guard guard;
    if (!Storage.openFileForRead("FWUPD", kFirmwareBinPath, firmwareFile)) {
      LOG_ERR("FWUPD", "Failed to open firmware file");
      return false;
    }
    firmwareSize = firmwareFile.size();
  }

  if (firmwareSize == 0) {
    LOG_ERR("FWUPD", "Firmware file is empty");
    SpiBusMutex::Guard guard;
    firmwareFile.close();
    return false;
  }

  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (!updatePartition) {
    LOG_ERR("FWUPD", "No OTA partition available");
    SpiBusMutex::Guard guard;
    firmwareFile.close();
    return false;
  }

  if (firmwareSize > updatePartition->size) {
    LOG_ERR("FWUPD", "Firmware file size (%zu) exceeds partition size (%zu)", firmwareSize, updatePartition->size);
    SpiBusMutex::Guard guard;
    firmwareFile.close();
    return false;
  }

  esp_ota_handle_t updateHandle = 0;
  // OTA_WITH_SEQUENTIAL_WRITES: erase sectors on demand
  esp_err_t err = esp_ota_begin(updatePartition, OTA_WITH_SEQUENTIAL_WRITES, &updateHandle);
  if (err != ESP_OK) {
    LOG_ERR("FWUPD", "esp_ota_begin failed: %s", esp_err_to_name(err));
    SpiBusMutex::Guard guard;
    firmwareFile.close();
    return false;
  }

  const size_t bufferSize = 4096;
  uint8_t* buffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (!buffer) {
    LOG_ERR("FWUPD", "Failed to allocate buffer");
    esp_ota_abort(updateHandle);
    SpiBusMutex::Guard guard;
    firmwareFile.close();
    return false;
  }

  size_t bytesRead = 0;
  size_t totalBytesWritten = 0;
  int lastProgress = -1;

  while (totalBytesWritten < firmwareSize) {
    {
      SpiBusMutex::Guard guard;
      bytesRead = firmwareFile.read(buffer, bufferSize);
    }

    if (bytesRead == 0) break;

    err = esp_ota_write(updateHandle, buffer, bytesRead);
    if (err != ESP_OK) {
      LOG_ERR("FWUPD", "esp_ota_write failed: %s", esp_err_to_name(err));
      free(buffer);
      esp_ota_abort(updateHandle);
      SpiBusMutex::Guard guard;
      firmwareFile.close();
      return false;
    }

    totalBytesWritten += bytesRead;

    int progress = (totalBytesWritten * 100) / firmwareSize;
    if (progress != lastProgress) {
      lastProgress = progress;
      LOG_INF("FWUPD", "Progress: %d%%", progress);

      // Display progress
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 200, tr(STR_UPDATING), true, EpdFontFamily::BOLD);
      char progressText[32];
      snprintf(progressText, sizeof(progressText), "%d%%", progress);
      renderer.drawCenteredText(UI_10_FONT_ID, 240, progressText, true);

      // Draw progress bar
      const int barWidth = 300;
      const int barHeight = 20;
      const int barX = (renderer.getScreenWidth() - barWidth) / 2;
      const int barY = 270;
      renderer.drawRect(barX, barY, barWidth, barHeight);
      renderer.fillRect(barX, barY, (barWidth * progress) / 100, barHeight);

      renderer.displayBuffer();
    }
  }

  free(buffer);
  {
    SpiBusMutex::Guard guard;
    firmwareFile.close();
  }

  if (totalBytesWritten != firmwareSize) {
    LOG_ERR("FWUPD", "Firmware write incomplete: %zu / %zu", totalBytesWritten, firmwareSize);
    esp_ota_abort(updateHandle);
    return false;
  }

  err = esp_ota_end(updateHandle);
  if (err != ESP_OK) {
    LOG_ERR("FWUPD", "esp_ota_end failed: %s", esp_err_to_name(err));
    return false;
  }

  err = esp_ota_set_boot_partition(updatePartition);
  if (err != ESP_OK) {
    LOG_ERR("FWUPD", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
    return false;
  }

  LOG_INF("FWUPD", "Firmware update successful. Deleting %s and rebooting...", kFirmwareBinPath);

  // Success! Delete the file so we don't update again on next boot.
  {
    SpiBusMutex::Guard guard;
    Storage.remove(kFirmwareBinPath);
  }

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 200, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, 240, tr(STR_BOOTING), true);
  renderer.displayBuffer();

  delay(2000);
  esp_restart();

  return true;
}
