#pragma once

#include "esp_err.h"
#include "esp_partition.h"

inline const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *) {
  return nullptr;
}

inline esp_err_t esp_ota_mark_app_valid_cancel_rollback() {
  return ESP_OK;
}
