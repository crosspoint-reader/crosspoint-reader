#include "features/ota_updates/Registration.h"

#include <ArduinoJson.h>
#include <FeatureFlags.h>
#include <WebServer.h>
#include <WiFi.h>

#include "activities/settings/OtaUpdateActivity.h"
#include "core/features/FeatureCatalog.h"
#include "core/registries/SettingsActionRegistry.h"
#include "core/registries/WebRouteRegistry.h"
#include "network/OtaWebCheck.h"

namespace features::ota_updates {
namespace {

#if ENABLE_OTA_UPDATES
bool shouldRegisterOtaApiRoute() { return core::FeatureCatalog::isEnabled("ota_updates"); }

void mountOtaRoutes(WebServer* server) {
  server->on("/api/ota/check", HTTP_POST, [server] {
    if (WiFi.status() != WL_CONNECTED) {
      server->send(503, "application/json", "{\"status\":\"error\",\"message\":\"Not connected to WiFi\"}");
      return;
    }
    switch (network::OtaWebCheck::start()) {
      case network::OtaWebStartResult::AlreadyChecking:
        server->send(200, "application/json", "{\"status\":\"checking\"}");
        return;
      case network::OtaWebStartResult::Started:
        server->send(202, "application/json", "{\"status\":\"checking\"}");
        return;
      case network::OtaWebStartResult::StartTaskFailed:
        server->send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to start task\"}");
        return;
      case network::OtaWebStartResult::Disabled:
        server->send(404, "application/json", "{\"status\":\"error\",\"message\":\"OTA API disabled\"}");
        return;
    }
  });

  server->on("/api/ota/check", HTTP_GET, [server] {
    const auto status = network::OtaWebCheck::getSnapshot();
    if (status.status == network::OtaWebCheckStatus::Disabled) {
      server->send(404, "application/json", "{\"status\":\"error\",\"message\":\"OTA API disabled\"}");
      return;
    }

    JsonDocument doc;
    doc["currentVersion"] = CROSSPOINT_VERSION;

    if (status.status == network::OtaWebCheckStatus::Checking) {
      doc["status"] = "checking";
    } else if (status.status == network::OtaWebCheckStatus::Done) {
      doc["status"] = "done";
      doc["available"] = status.available;
      doc["latestVersion"] = status.latestVersion.c_str();
      doc["latest_version"] = status.latestVersion.c_str();
      doc["message"] = status.message.c_str();
      doc["errorCode"] = status.errorCode;
      doc["error_code"] = status.errorCode;
    } else {
      doc["status"] = "idle";
    }

    String json;
    serializeJson(doc, json);
    server->send(200, "application/json", json);
  });
}
#endif

bool isSettingsActionSupported() { return core::FeatureCatalog::isEnabled("ota_updates"); }

Activity* createSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, void* callbackCtx,
                                 void (*onComplete)(void* ctx), void (*onCompleteBool)(void* ctx, bool result)) {
  (void)callbackCtx;
  (void)onComplete;
  (void)onCompleteBool;
  return new OtaUpdateActivity(renderer, mappedInput);
}

}  // namespace

void registerFeature() {
#if ENABLE_OTA_UPDATES
  core::SettingsActionEntry settingsEntry{};
  settingsEntry.action = SettingAction::CheckForUpdates;
  settingsEntry.isSupported = isSettingsActionSupported;
  settingsEntry.create = createSettingsActivity;
  core::SettingsActionRegistry::add(settingsEntry);

  core::WebRouteEntry webRouteEntry{};
  webRouteEntry.routeId = "ota_api";
  webRouteEntry.shouldRegister = shouldRegisterOtaApiRoute;
  webRouteEntry.mountRoutes = mountOtaRoutes;
  core::WebRouteRegistry::add(webRouteEntry);
#endif
}

}  // namespace features::ota_updates
