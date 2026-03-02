#include "HalNetwork.h"

#include <WiFi.h>

HalNetwork network;  // Singleton instance

void HalNetwork::enable() { WiFi.mode(WIFI_STA); }

bool HalNetwork::isActive() const { return WiFi.getMode() != WIFI_OFF; }

bool HalNetwork::isConnected() const {
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void HalNetwork::disable() {
  if (WiFi.getMode() == WIFI_OFF) {
    return;
  }
  WiFi.disconnect(false);  // false = don't erase credentials, send disconnect frame
  delay(100);              // Allow disconnect frame to be sent
  WiFi.mode(WIFI_OFF);
  delay(100);  // Allow WiFi hardware to fully power down
}
