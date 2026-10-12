#include "WifiApChoice.h"

#include <HalSystem.h>  // feedWatchdog()
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <cstdio>
#include <cstring>

#include "WifiCredentialStore.h"
#include "network/MacFormat.h"

namespace WifiApChoice {
namespace {

// Two beacon intervals (2 x 102.4 ms): every AP on a channel sends a beacon within it, so a
// passive scan hears an AP whether or not it answers probes.
constexpr uint32_t LISTEN_MS_PER_CHANNEL = 220;
constexpr unsigned long MOVE_TIMEOUT_MS = 8000;
constexpr int MAX_APS = 8;

struct Ap {
  uint8_t bssid[6] = {0, 0, 0, 0, 0, 0};
  int channel = 0;
  int rssi = 0;
};

std::string describe(const Ap& ap) {
  char text[48];
  snprintf(text, sizeof(text), "%s ch %d at %d dBm", formatMacDashed(ap.bssid).c_str(), ap.channel, ap.rssi);
  return text;
}

bool sameAp(const uint8_t a[6], const uint8_t b[6]) { return std::memcmp(a, b, 6) == 0; }

// The APs of `ssid` a passive scan hears on `channel` (0: every channel), one entry per BSSID.
int listen(const std::string& ssid, const int channel, Ap* out) {
  HalSystem::feedWatchdog();
  const int16_t n = WiFi.scanNetworks(false, false, true, LISTEN_MS_PER_CHANNEL, channel, ssid.c_str());
  HalSystem::feedWatchdog();
  int count = 0;
  for (int i = 0; i < n; i++) {
    const uint8_t* bssid = WiFi.BSSID(i);
    if (!bssid || WiFi.SSID(i) != ssid.c_str()) continue;
    Ap* known = nullptr;
    for (int k = 0; k < count; k++) {
      if (sameAp(out[k].bssid, bssid)) known = &out[k];
    }
    if (known) {
      if (WiFi.RSSI(i) > known->rssi) known->rssi = WiFi.RSSI(i);
      continue;
    }
    if (count == MAX_APS) continue;
    std::memcpy(out[count].bssid, bssid, 6);
    out[count].channel = WiFi.channel(i);
    out[count].rssi = WiFi.RSSI(i);
    count++;
  }
  WiFi.scanDelete();
  return count;
}

// Waits for an address on `bssid`, or on any AP when it is null. Fails on another AP: the core
// reconnected somewhere by itself.
bool waitConnected(const uint8_t* bssid) {
  const unsigned long start = millis();
  while (millis() - start < MOVE_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      const uint8_t* now = WiFi.BSSID();
      return !bssid || (now && sameAp(now, bssid));
    }
    HalSystem::feedWatchdog();
    delay(50);
  }
  return false;
}

// Leaves the AP and waits until the status says so, so waitConnected() cannot mistake the link
// just left for the one asked for.
void leave() {
  esp_wifi_disconnect();
  const unsigned long start = millis();
  while (WiFi.status() == WL_CONNECTED && millis() - start < 1000) delay(20);
}

// Hands the driver a station config, waiting out a connect it is busy with. The core reconnects
// once after the first disconnect since boot, whatever the reason and whatever setAutoReconnect()
// says ("Retry once for all failure reasons", first_connect in STA.cpp), and while that runs the
// driver refuses a config ("sta is connecting, cannot set config", ESP_ERR_WIFI_STATE). That is how
// the first move on a device failed: it left the far AP, the core's retry took the radio, the
// pinned config was refused, and the reader ended with no link at all.
bool applyConfig(wifi_config_t conf) {
  for (int attempt = 0; attempt < 10; attempt++) {
    const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &conf);
    if (err != ESP_ERR_WIFI_STATE) return err == ESP_OK;
    leave();
    delay(100);
  }
  return false;
}

// Moves to `to`, an AP of the network the reader is connected to: the driver's own config holds
// its name and password. Pinned to the BSSID just heard, with a fast scan of its channel; the pin
// is dropped again afterwards, so later reconnects - the driver's own ones included - pick the
// strongest AP. When the move fails the reader reconnects as a connect does. The core's own
// reconnects are off meanwhile.
bool moveTo(const Ap& to, const char* tag) {
  wifi_config_t pinned;
  if (esp_wifi_get_config(WIFI_IF_STA, &pinned) != ESP_OK) return false;
  wifi_config_t unpinned = pinned;
  unpinned.sta.bssid_set = 0;
  std::memset(unpinned.sta.bssid, 0, sizeof(unpinned.sta.bssid));
  unpinned.sta.channel = 0;
  unpinned.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  unpinned.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  pinned.sta.bssid_set = 1;
  std::memcpy(pinned.sta.bssid, to.bssid, 6);
  pinned.sta.channel = static_cast<uint8_t>(to.channel);
  pinned.sta.scan_method = WIFI_FAST_SCAN;

  WiFi.setAutoReconnect(false);
  leave();
  bool moved = applyConfig(pinned) && esp_wifi_connect() == ESP_OK && waitConnected(to.bssid);
  if (moved) {
    // Set while connected, it applies to the next connect.
    if (!applyConfig(unpinned)) LOG_DBG(tag, "Could not unpin the AP: a reconnect will look for it alone");
  } else {
    LOG_DBG(tag, "Could not move to %s; connecting as before", describe(to).c_str());
    leave();
    if (!applyConfig(unpinned) || esp_wifi_connect() != ESP_OK || !waitConnected(nullptr)) {
      LOG_DBG(tag, "No link yet after the failed move; the driver keeps trying");
    }
  }
  WiFi.setAutoReconnect(true);
  return moved;
}

}  // namespace

Result improve(const std::string& ssid, const char* tag) {
  Result result;
  const uint8_t* joined = WiFi.BSSID();
  if (WiFi.status() != WL_CONNECTED || !joined) return result;
  Ap current;
  std::memcpy(current.bssid, joined, 6);
  current.channel = WiFi.channel();
  current.rssi = WiFi.RSSI();
  if (current.rssi >= WEAK_RSSI) return result;

  const WifiCredential* cred = WIFI_STORE.findCredential(ssid);
  const uint8_t knownAps = cred ? cred->apCount : 0;
  if (knownAps == 1) {
    LOG_DBG(tag, "Weak signal from %s, but %s has a single access point", describe(current).c_str(), ssid.c_str());
    return result;
  }

  Ap heard[MAX_APS];
  const Ap* best = nullptr;
  // A known mesh whose last session ended on another AP: listen on that AP's channel first.
  if (knownAps >= 2 && cred->channel != 0 && !sameAp(cred->bssid, current.bssid)) {
    const int n = listen(ssid, cred->channel, heard);
    for (int i = 0; i < n; i++) {
      if (sameAp(heard[i].bssid, cred->bssid) && heard[i].rssi >= current.rssi + BETTER_BY_DB) best = &heard[i];
    }
    LOG_DBG(tag, "Listened on ch %d for the AP %s ended on last time: %s", cred->channel, ssid.c_str(),
            best ? describe(*best).c_str() : "not stronger, or not heard");
  }

  if (!best) {
    const int n = listen(ssid, 0, heard);
    bool joinedHeard = false;
    for (int i = 0; i < n; i++) {
      LOG_DBG(tag, "Heard %s from %s", ssid.c_str(), describe(heard[i]).c_str());
      if (sameAp(heard[i].bssid, current.bssid)) {
        joinedHeard = true;
      } else if (!best || heard[i].rssi > best->rssi) {
        best = &heard[i];
      }
    }
    // Nothing heard, not even the AP the reader is on: the scan failed, and teaches nothing.
    if (n > 0) result.apsHeard = static_cast<uint8_t>(n + (joinedHeard ? 0 : 1));
    if (best && best->rssi < current.rssi + BETTER_BY_DB) best = nullptr;
  }

  if (!best) {
    LOG_DBG(tag, "Staying on %s: no access point of %s is %d dB stronger", describe(current).c_str(), ssid.c_str(),
            BETTER_BY_DB);
    return result;
  }
  LOG_DBG(tag, "Moving from %s to %s", describe(current).c_str(), describe(*best).c_str());
  result.moved = moveTo(*best, tag);
  if (result.moved) LOG_DBG(tag, "Moved: %d dBm, ip=%s", WiFi.RSSI(), WiFi.localIP().toString().c_str());
  return result;
}

}  // namespace WifiApChoice
