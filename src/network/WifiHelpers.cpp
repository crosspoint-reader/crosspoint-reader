#include "WifiHelpers.h"

#include <Arduino.h>
#include <WiFi.h>

namespace WifiHelpers {

void wifiOn() {
  WiFi.mode(WIFI_STA);
}

void wifiOff() {
  WiFi.disconnect(false);  // false = don't erase credentials, send disconnect frame
  delay(100);              // Allow disconnect frame to be sent
  WiFi.mode(WIFI_OFF);
  delay(100);  // Allow WiFi hardware to fully power down
}

}  // namespace WifiHelpers
