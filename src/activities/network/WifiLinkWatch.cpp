#include "WifiLinkWatch.h"

#include <Logging.h>

void WifiLinkWatch::start() {
  if (logger) return;
  // Runs on the WiFi event task. The tag is a string literal, so the copy is safe to keep.
  const char* const logTag = tag;
  logger = WiFi.onEvent(
      [logTag](arduino_event_id_t, arduino_event_info_t info) {
        [[maybe_unused]] const uint8_t reason = info.wifi_sta_disconnected.reason;
        LOG_DBG(logTag, "WiFi dropped: reason=%u (%s) rssi=%d", reason,
                WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(reason)),
                static_cast<int>(info.wifi_sta_disconnected.rssi));
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
    const int rssi = WiFi.RSSI();
    if (lostAt != 0) {
      const String current = WiFi.localIP().toString();
      LOG_DBG(tag, "WiFi back after %lu ms: ip=%s rssi=%d", now - lostAt, current.c_str(), rssi);
      if (ip != current.c_str()) {
        LOG_DBG(tag, "The address changed from %s to %s", ip.c_str(), current.c_str());
        ip = current.c_str();
      }
      lostAt = 0;
    }
    // A weak link is worth knowing about, not every 2 s.
    if (rssi < -75 && (lastWeakLog == 0 || now - lastWeakLog >= 30000)) {
      LOG_DBG(tag, "Weak WiFi signal: %d dBm", rssi);
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
