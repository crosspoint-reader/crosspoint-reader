#include "UsbFlashWriter.h"

#include <Arduino.h>
#include <esp_ota_ops.h>

#include <algorithm>
#include <cstring>

#include "OtaBootSwitch.h"
namespace usb_update {
bool FlashWriter::begin(const esp_partition_t* target, uint32_t size, const uint8_t sha[32]) {
  const auto* running = esp_ota_get_running_partition();
  if (!target || !running || target->address == running->address) {
    result = firmware_flash::Result::NO_PARTITION;
    return false;
  }
  if (size < 65536 || size > target->size) {
    result = firmware_flash::Result::BAD_SIZE;
    return false;
  }
  destination = target;
  declared = size;
  written = erased = 0;
  memcpy(expected, sha, 32);
  result = firmware_flash::Result::OK;
  eraseMs = writeMs = verifyMs = selectMs = eraseCount = writeCount = maxEraseMs = maxWriteMs = 0;
  return true;
}
bool FlashWriter::append(const uint8_t* data, size_t n) {
  using firmware_flash::Result;
  if (!destination || result != Result::OK || !n || n > 32768 || written > declared || n > declared - written) {
    result = Result::BAD_SIZE;
    return false;
  }
  while (erased < written + n) {
    // Prefer block erases; a small final partition tail uses sector alignment.
    const size_t length = std::min<size_t>(65536, destination->size - erased);
    if (!length || length % 4096) {
      result = Result::ERASE_FAIL;
      return false;
    }
    const uint32_t start = millis();
    const int status = esp_partition_erase_range(destination, erased, length);
    const uint32_t ms = millis() - start;
    eraseMs += ms;
    maxEraseMs = std::max(maxEraseMs, ms);
    eraseCount++;
    if (status != ESP_OK) {
      result = Result::ERASE_FAIL;
      return false;
    }
    erased += length;
  }
  const uint32_t start = millis();
  const int status = esp_partition_write(destination, written, data, n);
  const uint32_t ms = millis() - start;
  writeMs += ms;
  maxWriteMs = std::max(maxWriteMs, ms);
  writeCount++;
  if (status != ESP_OK) {
    result = Result::WRITE_FAIL;
    return false;
  }
  written += n;
  return true;
}
bool FlashWriter::finish() {
  using firmware_flash::Result;
  if (!destination || result != Result::OK || written != declared) return false;
  const uint32_t verifyStart = millis();
  result = firmware_flash::validateImagePartition(destination, declared, expected);
  verifyMs = millis() - verifyStart;
  if (result != Result::OK) return false;
  const uint32_t selectStart = millis();
  const bool ok = ota_boot::switchTo(destination);
  selectMs = millis() - selectStart;
  if (!ok) result = Result::OTADATA_FAIL;
  destination = nullptr;
  return ok;
}
}  // namespace usb_update
