#pragma once

// NVS on the host: every read misses and every write is dropped, so the wake
// latch simply never persists. Same observable behaviour as the stubs this
// replaces, without guarding main.cpp.

#include <cstdint>

using nvs_handle_t = int;

enum { NVS_READONLY = 0, NVS_READWRITE = 1 };

#ifndef ESP_OK
using esp_err_t = int;
enum { ESP_OK = 0, ESP_FAIL = -1 };
#endif

inline esp_err_t nvs_open(const char*, int, nvs_handle_t*) { return ESP_FAIL; }
inline esp_err_t nvs_get_u8(nvs_handle_t, const char*, uint8_t*) { return ESP_FAIL; }
inline esp_err_t nvs_set_u8(nvs_handle_t, const char*, uint8_t) { return ESP_FAIL; }
inline esp_err_t nvs_commit(nvs_handle_t) { return ESP_FAIL; }
inline void nvs_close(nvs_handle_t) {}
