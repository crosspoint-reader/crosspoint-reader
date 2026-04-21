#pragma once

// Shared WiFi lifecycle helpers used by sync activities.
namespace WifiUtils {

// Disconnect WiFi and disable the radio. Stops SNTP first if running.
// Safe to call when already disconnected/off.
void wifiOff();

// Synchronise device clock via NTP (blocks up to 5 seconds).
void syncTimeWithNTP();

}  // namespace WifiUtils
