#include "features/remote_keyboard_input/Registration.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FeatureFlags.h>
#include <WebServer.h>

#include "core/features/FeatureCatalog.h"
#include "core/registries/WebRouteRegistry.h"
#include "network/RemoteKeyboardSession.h"
#include "network/WebUtils.h"
#include "network/html/RemoteKeyboardPageHtml.generated.h"

namespace features::remote_keyboard_input {
namespace {

#if ENABLE_REMOTE_KEYBOARD_INPUT
bool shouldRegisterRemoteKeyboardRoutes() { return core::FeatureCatalog::isEnabled("remote_keyboard_input"); }

void sendSessionSnapshot(WebServer* server) {
  const auto snapshot = REMOTE_KEYBOARD_SESSION.snapshot();

  JsonDocument doc;
  doc["active"] = snapshot.active;
  if (snapshot.active) {
    doc["id"] = snapshot.id;
    doc["title"] = snapshot.title;
    doc["text"] = snapshot.text;
    doc["maxLength"] = snapshot.maxLength;
    doc["isPassword"] = snapshot.isPassword;
    if (!snapshot.claimedBy.empty()) {
      doc["claimedBy"] = snapshot.claimedBy;
      doc["lastClaimAt"] = snapshot.lastClaimAt;
    }
  }

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void mountRemoteKeyboardRoutes(WebServer* server) {
  server->on("/remote-input", HTTP_GET,
             [server] { sendPrecompressedHtml(server, RemoteKeyboardPageHtml, RemoteKeyboardPageHtmlCompressedSize); });

  server->on("/api/remote-keyboard/session", HTTP_GET, [server] { sendSessionSnapshot(server); });

  server->on("/api/remote-keyboard/claim", HTTP_POST, [server] {
    if (!server->hasArg("plain")) {
      server->send(400, "text/plain", "Missing body");
      return;
    }

    const String body = server->arg("plain");
    JsonDocument doc;
    if (deserializeJson(doc, body.c_str())) {
      server->send(400, "text/plain", "Invalid JSON body");
      return;
    }

    const uint32_t id = doc["id"] | 0;
    const char* client = doc["client"] | "browser";
    if (id == 0 || !REMOTE_KEYBOARD_SESSION.claim(id, client)) {
      server->send(404, "text/plain", "Remote keyboard session not found");
      return;
    }

    sendSessionSnapshot(server);
  });

  server->on("/api/remote-keyboard/submit", HTTP_POST, [server] {
    if (!server->hasArg("plain")) {
      server->send(400, "text/plain", "Missing body");
      return;
    }

    const String body = server->arg("plain");
    JsonDocument doc;
    if (deserializeJson(doc, body.c_str())) {
      server->send(400, "text/plain", "Invalid JSON body");
      return;
    }

    const uint32_t id = doc["id"] | 0;
    const char* text = doc["text"] | "";
    switch (REMOTE_KEYBOARD_SESSION.submit(id, text)) {
      case RemoteKeyboardSession::SubmitResult::Submitted:
        server->send(200, "application/json", "{\"ok\":true}");
        return;
      case RemoteKeyboardSession::SubmitResult::TextTooLong:
        server->send(400, "text/plain", "Text exceeds session length limit");
        return;
      case RemoteKeyboardSession::SubmitResult::InvalidSession:
      default:
        server->send(404, "text/plain", "Remote keyboard session not found");
        return;
    }
  });
}
#endif

}  // namespace

void registerFeature() {
#if ENABLE_REMOTE_KEYBOARD_INPUT
  core::WebRouteEntry webRouteEntry{};
  webRouteEntry.routeId = "remote_keyboard_input_api";
  webRouteEntry.shouldRegister = shouldRegisterRemoteKeyboardRoutes;
  webRouteEntry.mountRoutes = mountRemoteKeyboardRoutes;
  core::WebRouteRegistry::add(webRouteEntry);
#endif
}

}  // namespace features::remote_keyboard_input
