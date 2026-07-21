#pragma once

#include "esp_http_client.h"

#define ESP_ERR_HTTPS_OTA_IN_PROGRESS -100

typedef void *esp_https_ota_handle_t;

struct esp_https_ota_config_t {
  const esp_http_client_config_t *http_config = nullptr;
  esp_err_t (*http_client_init_cb)(esp_http_client_handle_t) = nullptr;
};

inline esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *,
                                     esp_https_ota_handle_t *out) {
  if (out)
    *out = nullptr;
  return ESP_FAIL;
}
inline esp_err_t esp_https_ota_perform(esp_https_ota_handle_t) {
  return ESP_FAIL;
}
inline int esp_https_ota_get_image_len_read(esp_https_ota_handle_t) {
  return 0;
}
inline bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t) {
  return false;
}
inline esp_err_t esp_https_ota_finish(esp_https_ota_handle_t) { return ESP_OK; }
