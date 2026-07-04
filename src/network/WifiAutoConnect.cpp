#include "WifiAutoConnect.h"

#include <Logging.h>
#include <WiFi.h>

#include "WifiCredentialStore.h"

namespace {

constexpr unsigned long kConnectionTimeoutMs = 15000;

void configureWifiSta() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String hostname = "CrossPoint-Reader-" + mac;
  WiFi.setHostname(hostname.c_str());
}

}  // namespace

WifiAutoConnectResult connectLastSaved(std::string& ssidOut) {
  ssidOut.clear();

  WIFI_STORE.loadFromFile();
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) {
    LOG_DBG("WIFI", "Auto-connect: no last connected SSID");
    return WifiAutoConnectResult::NoCredentials;
  }

  const WifiCredential* cred = WIFI_STORE.findCredential(lastSsid);
  if (cred == nullptr) {
    LOG_DBG("WIFI", "Auto-connect: no saved credential for %s", lastSsid.c_str());
    return WifiAutoConnectResult::NoCredentials;
  }

  ssidOut = cred->ssid;
  const bool requiresPassword = !cred->password.empty();
  LOG_DBG("WIFI", "Auto-connect: attempting %s", ssidOut.c_str());

  configureWifiSta();

  if (requiresPassword) {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str());
  }

  const unsigned long startMs = millis();
  while (millis() - startMs < kConnectionTimeoutMs) {
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) {
      WIFI_STORE.setLastConnectedSsid(cred->ssid);
      LOG_DBG("WIFI", "Auto-connect: connected to %s", ssidOut.c_str());
      return WifiAutoConnectResult::Connected;
    }
    if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
      WiFi.disconnect();
      LOG_DBG("WIFI", "Auto-connect: failed for %s (status=%d)", ssidOut.c_str(), static_cast<int>(status));
      return WifiAutoConnectResult::Failed;
    }
    delay(100);
  }

  WiFi.disconnect();
  LOG_DBG("WIFI", "Auto-connect: timeout for %s", ssidOut.c_str());
  return WifiAutoConnectResult::Timeout;
}
