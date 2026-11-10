#include "DeviceSecret.h"

#include <Logging.h>
#include <esp_random.h>
#include <nvs.h>

#include <cstring>
#include <mutex>

bool deviceSecret(uint8_t (&out)[32]) {
  static uint8_t secret[32];
  static bool loaded = false;
  static std::mutex lock;  // web-server task and reader can both ask first
  std::lock_guard<std::mutex> guard(lock);

  // A failure is not cached: the next call retries.
  if (!loaded) {
    nvs_handle_t handle;
    if (nvs_open("devid", NVS_READWRITE, &handle) == ESP_OK) {
      size_t stored = sizeof(secret);
      const esp_err_t err = nvs_get_blob(handle, "secret", secret, &stored);
      if (err == ESP_OK) {
        loaded = stored == sizeof(secret);
      } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Only a missing secret is created. Any other read error leaves the
        // stored one alone: replacing it would orphan every wrapped book key.
        esp_fill_random(secret, sizeof(secret));
        loaded = nvs_set_blob(handle, "secret", secret, sizeof(secret)) == ESP_OK && nvs_commit(handle) == ESP_OK;
      }
      nvs_close(handle);
    }
    if (!loaded) {
      LOG_ERR("DSEC", "Device secret unavailable");
      return false;
    }
  }
  memcpy(out, secret, sizeof(out));
  return true;
}
