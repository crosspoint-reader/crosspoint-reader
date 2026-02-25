#pragma once

namespace WifiHelpers {

/**
 * Enable WiFi in STA (station) mode.
 */
void wifiOn();

/**
 * Gracefully disconnect and power down WiFi.
 * Note: callers are responsible for stopping any services that depend on
 * the network (e.g. SNTP, mDNS) before calling this.
 */
void wifiOff();

}  // namespace WifiHelpers
