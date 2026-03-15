#include "network/RemoteKeyboardNetworkSession.h"

#include <WiFi.h>

#include "activities/ActivityManager.h"
#include "network/BackgroundWebServer.h"
#include "network/BackgroundWifiService.h"
#include "network/CrossPointWebServer.h"
#include "util/NetworkNames.h"

namespace {
constexpr uint8_t kApChannel = 1;
constexpr uint8_t kApMaxConnections = 4;

bool hasStaWifiConnection() {
  const wifi_mode_t wifiMode = WiFi.getMode();
  if ((wifiMode & WIFI_MODE_STA) == 0) {
    return false;
  }
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}
}  // namespace

RemoteKeyboardNetworkSession::~RemoteKeyboardNetworkSession() = default;

bool RemoteKeyboardNetworkSession::begin() {
  end();

  if (hasStaWifiConnection()) {
    return startServerOnCurrentConnection();
  }

  return startAccessPointAndServer();
}

void RemoteKeyboardNetworkSession::loop() {
  if (ownedServer && ownedServer->isRunning()) {
    ownedServer->handleClient();
  }
}

void RemoteKeyboardNetworkSession::end() {
  if (ownedServer) {
    ownedServer->stop();
    ownedServer.reset();
  }

  if (startedAp) {
    WiFi.softAPdisconnect(true);
    delay(30);
    WiFi.mode(WIFI_OFF);
    delay(30);
  }

  startedAp = false;
  state = {};
}

bool RemoteKeyboardNetworkSession::startServerOnCurrentConnection() {
  if (BG_WIFI.isRunning()) {
    BG_WIFI.stop(true);
  }

  state.apMode = false;
  state.ssid = WiFi.SSID().c_str();
  state.ip = WiFi.localIP().toString().c_str();
  state.url = "http://" + state.ip + "/remote-input";

  if (BackgroundWebServer::getInstance().isRunning()) {
    state.ready = true;
    state.usingExistingServer = true;
    return true;
  }

  ownedServer = std::make_unique<CrossPointWebServer>();
  ownedServer->begin();
  if (!ownedServer->isRunning()) {
    ownedServer.reset();
    state = {};
    return false;
  }

  state.ready = true;
  return true;
}

bool RemoteKeyboardNetworkSession::startAccessPointAndServer() {
  if (BG_WIFI.isRunning()) {
    BG_WIFI.stop(false);
  }

  WiFi.mode(WIFI_AP);
  delay(100);

  char apSsid[40];
  NetworkNames::getApSsid(apSsid, sizeof(apSsid));
  if (!WiFi.softAP(apSsid, nullptr, kApChannel, false, kApMaxConnections)) {
    WiFi.mode(WIFI_OFF);
    return false;
  }
  delay(100);

  startedAp = true;
  state.apMode = true;
  state.ssid = apSsid;
  state.ip = WiFi.softAPIP().toString().c_str();
  state.url = "http://" + state.ip + "/remote-input";

  ownedServer = std::make_unique<CrossPointWebServer>();
  ownedServer->begin();
  if (!ownedServer->isRunning()) {
    end();
    return false;
  }

  state.ready = true;
  return true;
}
