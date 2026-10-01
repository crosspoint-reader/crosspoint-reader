#include "DeviceSecret.h"

#include <Logging.h>
#include <Preferences.h>
#include <esp_random.h>

#include <cstring>
#include <mutex>

bool deviceSecret(uint8_t (&out)[32]) {
  static uint8_t secret[32];
  static bool loaded = false;
  static std::mutex lock;  // web-server task and reader can both ask first
  std::lock_guard<std::mutex> guard(lock);

  // A failure is not cached: the next call retries.
  if (!loaded) {
    Preferences prefs;
    if (prefs.begin("devid", false)) {
      const size_t stored = prefs.getBytesLength("secret");
      if (stored == sizeof(secret)) {
        loaded = prefs.getBytes("secret", secret, sizeof(secret)) == sizeof(secret);
      } else if (stored == 0) {
        // Only a missing secret is created. Replacing an existing one would
        // orphan every book key wrapped with it.
        esp_fill_random(secret, sizeof(secret));
        loaded = prefs.putBytes("secret", secret, sizeof(secret)) == sizeof(secret);
      }
      prefs.end();
    }
    if (!loaded) {
      LOG_ERR("DSEC", "Device secret unavailable");
      return false;
    }
  }
  memcpy(out, secret, sizeof(out));
  return true;
}
