#include "WifiLinkWatch.h"

#include <Logging.h>

#include "network/MacFormat.h"
#include "network/WifiApChoice.h"

namespace {
// "-87 dBm from f0-9f-c2-31-b8-0c ch 11": the signal and the access point it comes from.
std::string signalFrom(const int rssi) {
  const uint8_t* bssid = WiFi.BSSID();
  char text[64];
  snprintf(text, sizeof(text), "%d dBm from %s ch %d", rssi, bssid ? formatMacDashed(bssid).c_str() : "?",
           static_cast<int>(WiFi.channel()));
  return text;
}
}  // namespace

void WifiLinkWatch::start() {
  if (logger) return;
  // Runs on the WiFi event task. The tag is a string literal, so the copy is safe to keep.
  const char* const logTag = tag;
  logger = WiFi.onEvent(
      [logTag](arduino_event_id_t, arduino_event_info_t info) {
        [[maybe_unused]] const uint8_t reason = info.wifi_sta_disconnected.reason;
        LOG_DBG(logTag, "WiFi dropped: reason=%u (%s) rssi=%d bssid=%s", reason,
                WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(reason)),
                static_cast<int>(info.wifi_sta_disconnected.rssi),
                formatMacDashed(info.wifi_sta_disconnected.bssid).c_str());
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
}

void WifiLinkWatch::stop() {
  if (!logger) return;
  WiFi.removeEvent(logger);
  logger = 0;
}

WifiLinkWatch::Link WifiLinkWatch::poll(std::string& ip) {
  const unsigned long now = millis();
  if (lastCheck != 0 && now - lastCheck < 2000) return link;
  lastCheck = now;

  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    if (lostAt != 0) {
      LOG_DBG(tag, "WiFi back after %lu ms: ip=%s, %s", now - lostAt, WiFi.localIP().toString().c_str(),
              signalFrom(rssi).c_str());
      // The driver's reconnect sweeps like a connect does, so on a mesh it too can land on a far AP.
      // Nothing is recorded here (no display lock for the SD card); the next connect's check is.
      if (rssi < WifiApChoice::WEAK_RSSI && WifiApChoice::improve(WiFi.SSID().c_str(), tag).moved) {
        rssi = WiFi.RSSI();
      }
      const String current = WiFi.localIP().toString();
      loggedRssi = rssi;
      if (ip != current.c_str()) {
        LOG_DBG(tag, "The address changed from %s to %s", ip.c_str(), current.c_str());
        ip = current.c_str();
      }
      lostAt = 0;
    }
    // The signal as the session starts, and each big change of it; a weak one also every 30 s.
    if (loggedRssi == 0 || abs(rssi - loggedRssi) >= SIGNAL_STEP_DB) {
      LOG_DBG(tag, "WiFi signal %s%s", signalFrom(rssi).c_str(), loggedRssi == 0 ? "" : " (changed)");
      loggedRssi = rssi;
      if (rssi < -75) lastWeakLog = now;
    } else if (rssi < -75 && (lastWeakLog == 0 || now - lastWeakLog >= 30000)) {
      LOG_DBG(tag, "Weak WiFi signal: %s", signalFrom(rssi).c_str());
      lastWeakLog = now;
    }
    link = Link::Up;
    return link;
  }

  if (lostAt == 0) {
    lostAt = now;
    LOG_DBG(tag, "WiFi lost (status %d); waiting up to %lu s for it to come back", status, GRACE_MS / 1000);
    link = Link::Reconnecting;
  } else if (link == Link::Reconnecting && now - lostAt >= GRACE_MS) {
    LOG_DBG(tag, "WiFi still gone after %lu ms (status %d)", now - lostAt, status);
    link = Link::Gone;
  }
  return link;
}
