#pragma once

#include <cstdint>
#include <string>

// Which access point of a network the reader sits on.
//
// On a mesh (several APs, one network name) a connect joins the strongest AP its sweep heard. The
// sweep listens 120 ms a channel (WifiSelectionActivity::applyScanBudget()), which does not always
// catch a beacon, and an AP that holds back probe answers (band steering) is then missed. The
// reader joins a far AP and stays there for the session: on a UniFi mesh (2026-10-12) about half
// the connects went to an AP at -79..-91 dBm while the one in the same room gave -47.
//
// improve() runs after a connect. When the AP is weak it listens longer, up to two beacon intervals
// a channel: actively, and passively as well when that is in doubt, so an AP that holds back probe
// answers is heard too. It moves the reader to a clearly stronger AP of the same network.
//
// It learns, through WifiCredentialStore, how many APs a network has (apCount: a single router is
// left alone once known) and which AP a session ended on (bssid/channel). On a known mesh it
// listens on that AP's channel first: a quarter of a second instead of three.
namespace WifiApChoice {

// Below this the reader looks for a better AP. A build flag can raise it so the check runs on every
// connect, for testing: -DWIFI_AP_WEAK_RSSI=-40.
#ifndef WIFI_AP_WEAK_RSSI
#define WIFI_AP_WEAK_RSSI -70
#endif
constexpr int WEAK_RSSI = WIFI_AP_WEAK_RSSI;
// A better AP must be at least this much stronger: no hopping between two that are about equal.
constexpr int BETTER_BY_DB = 10;

struct Result {
  // The APs of the network a full scan heard, the joined one included; 0 when there was no full
  // scan. The caller records it (WifiCredentialStore::noteApCount()).
  uint8_t apsHeard = 0;
  bool moved = false;
};

// After a connect to `ssid`. Returns at once when the AP is strong enough or the network is known
// to have a single one; otherwise blocks while it listens (up to ~7 s) and moves (~1 s usually,
// up to ~16 s when the move fails and it reconnects). It moves only to an AP a second, short
// listen on its channel hears again. Logs what it heard and did under `tag`.
// Leaves the connection unpinned, so the driver's own reconnects pick the strongest AP. A failed
// move can leave no link yet: the caller checks WiFi.status() afterwards.
Result improve(const std::string& ssid, const char* tag);

}  // namespace WifiApChoice
