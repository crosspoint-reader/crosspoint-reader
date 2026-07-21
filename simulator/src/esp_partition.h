#pragma once

#include "esp_err.h"

#include <cstddef>
#include <cstdint>

using esp_partition_type_t = uint8_t;
using esp_partition_subtype_t = uint8_t;

#define ESP_PARTITION_TYPE_DATA 0x01
#define ESP_PARTITION_SUBTYPE_DATA_OTA 0x00
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 0x10

struct esp_partition_t {
  esp_partition_type_t type = 0;
  esp_partition_subtype_t subtype = 0;
  uint32_t address = 0;
  uint32_t size = 0;
  char label[17] = {};
};

inline const esp_partition_t *esp_partition_find_first(esp_partition_type_t, esp_partition_subtype_t,
                                                       const char *) {
  return nullptr;
}

inline esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t) {
  return ESP_FAIL;
}

inline esp_err_t esp_partition_write(const esp_partition_t *, size_t, const void *, size_t) {
  return ESP_FAIL;
}

inline esp_err_t esp_partition_erase_range(const esp_partition_t *, size_t, size_t) {
  return ESP_FAIL;
}
