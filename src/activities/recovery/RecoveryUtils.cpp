#include "RecoveryUtils.h"

#include <Logging.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <cassert>

namespace Recovery {

void getPartitions(const esp_partition_t*& appPartition, const esp_partition_t*& recoveryPartition) {
  appPartition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  recoveryPartition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);

  if (appPartition == nullptr || recoveryPartition == nullptr) {
    LOG_ERR("REC", "Failed to find app or recovery partition");
    return;
  }

  LOG_INF("REC", "App partition: address=0x%08x, size=0x%08x", appPartition->address, appPartition->size);
  LOG_INF("REC", "Recovery partition: address=0x%08x, size=0x%08x", recoveryPartition->address, recoveryPartition->size);
}

void reboot(bool toRecovery) {
  const esp_partition_t* appPartition;
  const esp_partition_t* recoveryPartition;
  getPartitions(appPartition, recoveryPartition);
  assert(appPartition != nullptr && recoveryPartition != nullptr);

  // TODO: maybe verify header of recovery partition to make sure it's valid before booting into it?

  esp_ota_set_boot_partition(toRecovery ? recoveryPartition : appPartition);
  esp_restart();
}

}; // namespace Recovery
