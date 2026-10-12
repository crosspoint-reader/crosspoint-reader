#pragma once

#include <WiFi.h>

#include <string>

// The STA link of a network session (File Transfer, Calibre). The driver reconnects by itself
// after a drop, so a lost link gets GRACE_MS to come back before it counts as gone; why the access
// point dropped the reader, and each failed try to reconnect, are logged as they happen.
//
// The log names the access point (BSSID, channel) with the signal: the reader joins the strongest
// one for the network's name and never moves on by itself, so on a mesh a weak signal means
// either that AP fading or the reader sitting on a far one. Each change of SIGNAL_STEP_DB or more
// is logged, so the moment it fades shows.
//
// Hotspot sessions need none of this: the reader is the network there.
class WifiLinkWatch {
 public:
  enum class Link { Up, Reconnecting, Gone };
  static constexpr unsigned long GRACE_MS = 30000;
  static constexpr int SIGNAL_STEP_DB = 10;

  explicit WifiLinkWatch(const char* logTag) : tag(logTag) {}
  ~WifiLinkWatch() { stop(); }
  WifiLinkWatch(const WifiLinkWatch&) = delete;
  WifiLinkWatch& operator=(const WifiLinkWatch&) = delete;

  // Registers the drop logger, once the session is up.
  void start();
  // Unregisters it. Called before a deliberate disconnect, which it would only log.
  void stop();

  // Looks at the link every 2 s and returns its state, the last one in between. `ip` is the
  // address the session shows; it is updated when the link comes back on another one.
  Link poll(std::string& ip);

 private:
  const char* tag;
  wifi_event_id_t logger = 0;
  unsigned long lastCheck = 0;
  unsigned long lostAt = 0;  // 0 while the link is up
  unsigned long lastWeakLog = 0;
  int loggedRssi = 0;  // the signal the log last reported; 0 before the first report
  Link link = Link::Up;
};
