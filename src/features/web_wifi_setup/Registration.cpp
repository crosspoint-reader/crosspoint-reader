#include "features/web_wifi_setup/Registration.h"

#include <ArduinoJson.h>
#include <FeatureFlags.h>
#include <Logging.h>
#include <WebServer.h>
#include <WiFi.h>

#include "WifiCredentialStore.h"
#include "core/features/FeatureCatalog.h"
#include "core/registries/WebRouteRegistry.h"

namespace features::web_wifi_setup {
namespace {

#if ENABLE_WEB_WIFI_SETUP
bool shouldRegisterWebWifiSetupApiRoute() { return core::FeatureCatalog::isEnabled("web_wifi_setup"); }

void mountWifiRoutes(WebServer* server) {
  server->on("/api/wifi/scan", HTTP_GET, [server] {
    int n = WiFi.scanNetworks();
    const bool staConnected = (WiFi.getMode() & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
    const String activeSsid = staConnected ? WiFi.SSID() : String();
    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();
    for (int i = 0; i < n; ++i) {
      const String networkSsid = WiFi.SSID(i);
      const bool encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      JsonObject obj = array.add<JsonObject>();
      obj["ssid"] = networkSsid;
      obj["rssi"] = WiFi.RSSI(i);
      obj["encrypted"] = encrypted;
      obj["secured"] = encrypted;
      obj["saved"] = WIFI_STORE.hasSavedCredential(networkSsid.c_str());
      obj["connected"] = staConnected && (networkSsid == activeSsid);
    }
    WiFi.scanDelete();
    String json;
    serializeJson(doc, json);
    server->send(200, "application/json", json);
  });

  server->on("/api/wifi/connect", HTTP_POST, [server] {
    if (!server->hasArg("plain")) {
      server->send(400, "text/plain", "Missing body");
      return;
    }
    JsonDocument doc;
    deserializeJson(doc, server->arg("plain"));
    String ssid = doc["ssid"];
    String password = doc["password"];
    if (ssid.length() == 0) {
      server->send(400, "text/plain", "SSID required");
      return;
    }
    WIFI_STORE.addCredential(ssid.c_str(), password.c_str());
    WIFI_STORE.saveToFile();
    server->send(200, "text/plain", "WiFi credentials saved");
  });

  server->on("/api/wifi/forget", HTTP_POST, [server] {
    if (!server->hasArg("plain")) {
      server->send(400, "text/plain", "Missing body");
      return;
    }
    JsonDocument doc;
    deserializeJson(doc, server->arg("plain"));
    String ssid = doc["ssid"];
    if (ssid.length() > 0) {
      WIFI_STORE.removeCredential(ssid.c_str());
      WIFI_STORE.saveToFile();
      server->send(200, "text/plain", "WiFi credentials removed");
    } else {
      server->send(400, "text/plain", "SSID required");
    }
  });

  server->on("/api/wifi/status", HTTP_GET, [server] {
    JsonDocument doc;
    const wl_status_t wifiSt = WiFi.status();
    const bool connected = wifiSt == WL_CONNECTED;
    doc["connected"] = connected;
    if (WiFi.getMode() == WIFI_MODE_AP) {
      doc["mode"] = "AP";
      doc["ssid"] = WiFi.softAPSSID();
    } else if (connected) {
      doc["mode"] = "STA";
      doc["ssid"] = WiFi.SSID();
      doc["ip"] = WiFi.localIP().toString();
      doc["rssi"] = WiFi.RSSI();
    } else {
      doc["mode"] = "STA";
      const char* stStr = (wifiSt == WL_CONNECT_FAILED)  ? "failed"
                          : (wifiSt == WL_NO_SSID_AVAIL) ? "no_ssid"
                          : (wifiSt == WL_IDLE_STATUS)   ? "connecting"
                                                         : "disconnected";
      doc["status"] = stStr;
    }
    String json;
    serializeJson(doc, json);
    server->send(200, "application/json", json);
  });
}
#endif

}  // namespace

void registerFeature() {
#if ENABLE_WEB_WIFI_SETUP
  core::WebRouteEntry webRouteEntry{};
  webRouteEntry.routeId = "web_wifi_setup_api";
  webRouteEntry.shouldRegister = shouldRegisterWebWifiSetupApiRoute;
  webRouteEntry.mountRoutes = mountWifiRoutes;
  core::WebRouteRegistry::add(webRouteEntry);
#endif
}

}  // namespace features::web_wifi_setup
