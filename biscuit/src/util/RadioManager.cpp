#include "RadioManager.h"

#include <BLEDevice.h>
#include <Logging.h>
#include <Preferences.h>
#include <WiFi.h>

bool RadioManager::ensureWifi() {
  if (state == RadioState::WIFI) return true;

  if (state == RadioState::BLE) {
    deinitBle();
  }

  WiFi.mode(WIFI_STA);
  state = RadioState::WIFI;
  LOG_DBG("RADIO", "Switched to WiFi mode, heap: %d", ESP.getFreeHeap());
  return true;
}

bool RadioManager::ensureBle() {
  if (state == RadioState::BLE) return true;

  if (state == RadioState::WIFI) {
    deinitWifi();
  }

  BLEDevice::init("biscuit");
  state = RadioState::BLE;
  LOG_DBG("RADIO", "Switched to BLE mode, heap: %d", ESP.getFreeHeap());
  return true;
}

void RadioManager::shutdown() {
  if (state == RadioState::WIFI) deinitWifi();
  if (state == RadioState::BLE) deinitBle();
  state = RadioState::OFF;
}

void RadioManager::deinitWifi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  LOG_DBG("RADIO", "WiFi deinitialized");
}

void RadioManager::deinitBle() {
  BLEDevice::deinit(false);
  delay(50);
  LOG_DBG("RADIO", "BLE deinitialized");
}

bool RadioManager::isDisclaimerAcknowledged() const {
  Preferences prefs;
  prefs.begin("biscuit", true);
  bool ack = prefs.getBool("disc_ack", false);
  prefs.end();
  return ack;
}

void RadioManager::setDisclaimerAcknowledged() {
  Preferences prefs;
  prefs.begin("biscuit", false);
  prefs.putBool("disc_ack", true);
  prefs.end();
}
