#include "DeviceSecret.h"

#include <Logging.h>
#include <Preferences.h>
#include <esp_random.h>

#include <cstring>

bool deviceSecret(uint8_t (&out)[32]) {
  static uint8_t secret[32];
  static const bool ok = [] {
    Preferences prefs;
    if (!prefs.begin("devid", false)) return false;
    bool have = prefs.getBytes("secret", secret, sizeof(secret)) == sizeof(secret);
    if (!have) {
      esp_fill_random(secret, sizeof(secret));
      have = prefs.putBytes("secret", secret, sizeof(secret)) == sizeof(secret);
    }
    prefs.end();
    if (!have) LOG_ERR("DSEC", "Device secret unavailable");
    return have;
  }();
  if (ok) memcpy(out, secret, sizeof(out));
  return ok;
}
