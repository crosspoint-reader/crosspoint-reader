#include "BootSwitch.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <stdio.h>
#include <string.h>

#include "BootSwitchLog.h"
#include "OtaBootSwitch.h"

#ifndef ESP_APP_DESC_MAGIC_WORD
#define ESP_APP_DESC_MAGIC_WORD 0xABCD5432
#endif

namespace boot_switch {

namespace {

// The app descriptor sits at a fixed offset in every app image:
// sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) = 24 + 8.
constexpr size_t kAppDescOffset = 0x20;

// Reads a fixed-size string field of the flash-resident esp_app_desc_t into a
// caller buffer one field at a time — the full struct is 256 bytes, too large
// for a stack local under the project's frame-size rule.
bool readDescField(const esp_partition_t* part, size_t fieldOffset, char* out, size_t fieldLen) {
  if (esp_partition_read(part, kAppDescOffset + fieldOffset, out, fieldLen) != ESP_OK) {
    return false;
  }
  out[fieldLen] = '\0';  // caller buffers are fieldLen + 1
  return true;
}

const esp_partition_t* passivePartition() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* passive = esp_ota_get_next_update_partition(nullptr);
  if (!passive || passive == running) {
    BOOTSWITCH_LOG_ERR("BOOT", "no passive OTA slot");
    return nullptr;
  }
  return passive;
}

bool holdsPlausibleImage(const esp_partition_t* part) {
  uint8_t magic = 0;
  if (esp_partition_read(part, 0, &magic, sizeof(magic)) != ESP_OK) {
    BOOTSWITCH_LOG_ERR("BOOT", "read of %s failed", part->label);
    return false;
  }
  return magic == ota_boot::kEspImageMagic;
}

}  // namespace

bool peekPassiveSlot(PassiveSlotInfo& out) {
  memset(&out, 0, sizeof(out));

  const esp_partition_t* passive = passivePartition();
  if (!passive) return false;

  strncpy(out.label, passive->label, sizeof(out.label) - 1);

  if (!holdsPlausibleImage(passive)) {
    BOOTSWITCH_LOG_INF("BOOT", "passive slot %s holds no app image", passive->label);
    return false;
  }

  // Best-effort metadata: only trust the descriptor string when the magic
  // word checks out; a blank descriptor is not an error.
  uint32_t descMagic = 0;
  if (esp_partition_read(passive, kAppDescOffset, &descMagic, sizeof(descMagic)) == ESP_OK &&
      descMagic == ESP_APP_DESC_MAGIC_WORD) {
    readDescField(passive, offsetof(esp_app_desc_t, version), out.version, sizeof(out.version) - 1);
  }
  return true;
}

bool swapToPassive() {
  const esp_partition_t* passive = passivePartition();
  if (!passive) return false;

  if (!holdsPlausibleImage(passive)) {
    BOOTSWITCH_LOG_ERR("BOOT", "refusing swap: %s holds no app image", passive->label);
    return false;
  }

  return ota_boot::switchTo(passive);
}

void describeSlot(const PassiveSlotInfo& info, char* out, size_t outLen) {
  if (info.version[0] != '\0') {
    snprintf(out, outLen, "%s: %s", info.label, info.version);
  } else {
    snprintf(out, outLen, "%s", info.label);
  }
}

}  // namespace boot_switch
