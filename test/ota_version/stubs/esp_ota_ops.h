#pragma once

#include <OtaTestSupport.h>

#include <cstddef>

struct esp_partition_t {};
using esp_ota_handle_t = unsigned;
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr size_t OTA_SIZE_UNKNOWN = 0;

inline const esp_partition_t* esp_ota_get_next_update_partition(const void*) {
  static esp_partition_t partition;
  return &partition;
}
inline esp_err_t esp_ota_begin(const esp_partition_t*, size_t, esp_ota_handle_t*) {
  ++ota_test::flashBegins;
  return ESP_OK;
}
inline esp_err_t esp_ota_write(esp_ota_handle_t, const void*, size_t) { return ESP_OK; }
inline esp_err_t esp_ota_abort(esp_ota_handle_t) { return ESP_OK; }
inline esp_err_t esp_ota_end(esp_ota_handle_t) { return ESP_OK; }
inline esp_err_t esp_ota_set_boot_partition(const esp_partition_t*) { return ESP_OK; }
