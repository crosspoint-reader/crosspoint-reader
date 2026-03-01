#pragma once

#include <Arduino.h>

class HalNetwork;
extern HalNetwork network;  // Singleton

/**
 * HAL abstraction for network on/off control.
 *
 * Isolates callers from the underlying network hardware so that:
 *  - The WiFi driver (ESP32 Arduino WiFi) is referenced in one place only.
 *  - Future ports (e.g. X3) or an emulator stub can replace this class
 *    without touching any activity code.
 *
 * Currently wraps ESP32 WiFi STA mode. AP mode and connection management
 * (scanning, credentials, SSID selection) remain in the activity layer,
 * as they are application-level concerns rather than raw hardware control.
 */
class HalNetwork {
 public:
  /**
   * Enable the network interface in STA (station) mode.
   * The caller is responsible for subsequently connecting to an AP via
   * the WiFi library (WiFi.begin) or delegating to WifiSelectionActivity.
   */
  void enable();

  /**
   * Gracefully disconnect and power down the network interface.
   * Callers are responsible for stopping any services that depend on
   * the network (e.g. SNTP, mDNS, HTTP servers) before calling this.
   */
  void disable();

  /**
   * Returns true when the network interface is powered on in any mode
   * (STA, AP, or both).  Used by HalPowerManager to suppress CPU throttling
   * while the radio is active.
   */
  bool isActive() const;

  /**
   * Returns true when the interface is associated to an AP and has a valid
   * IP address.  Prefer this over raw WiFi.status() checks so that the
   * two-condition test (status + valid IP) is applied consistently everywhere.
   */
  bool isConnected() const;
};
