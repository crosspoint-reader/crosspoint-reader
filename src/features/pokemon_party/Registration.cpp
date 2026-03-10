#include "features/pokemon_party/Registration.h"

#include <ArduinoJson.h>
#include <FeatureFlags.h>
#include <HalStorage.h>
#include <WebServer.h>

#include "core/features/FeatureCatalog.h"
#include "core/registries/WebRouteRegistry.h"
#include "util/PathUtils.h"
#include "util/PokemonBookDataStore.h"

namespace features::pokemon_party {
namespace {

#if ENABLE_WEB_POKEDEX_PLUGIN
bool shouldRegisterPokemonPartyApiRoute() { return core::FeatureCatalog::isEnabled("pokemon_party"); }

void mountPokemonRoutes(WebServer* server) {
  server->on("/api/book-pokemon", HTTP_GET, [server] {
    if (!server->hasArg("path")) {
      server->send(400, "text/plain", "Missing path");
      return;
    }
    String bookPath = PathUtils::urlDecode(server->arg("path"));
    if (!PathUtils::isValidSdPath(bookPath)) {
      server->send(400, "text/plain", "Invalid path");
      return;
    }
    bookPath = PathUtils::normalizePath(bookPath);
    if (PathUtils::pathContainsProtectedItem(bookPath)) {
      server->send(403, "text/plain", "Cannot access protected items");
      return;
    }
    if (!Storage.exists(bookPath.c_str())) {
      server->send(404, "text/plain", "Book not found");
      return;
    }
    if (!PokemonBookDataStore::supportsBookPath(bookPath.c_str())) {
      server->send(400, "text/plain", "Unsupported book type");
      return;
    }
    JsonDocument response;
    response["path"] = bookPath;
    JsonDocument pokemonDoc;
    if (PokemonBookDataStore::loadPokemonDocument(bookPath.c_str(), pokemonDoc) &&
        pokemonDoc["pokemon"].is<JsonObject>()) {
      response["pokemon"] = pokemonDoc["pokemon"];
    } else {
      response["pokemon"] = nullptr;
    }
    String json;
    serializeJson(response, json);
    server->send(200, "application/json", json);
  });

  server->on("/api/book-pokemon", HTTP_PUT, [server] {
    if (!server->hasArg("plain")) {
      server->send(400, "text/plain", "Missing body");
      return;
    }
    const String requestBody = server->arg("plain");
    JsonDocument request;
    if (deserializeJson(request, requestBody.c_str())) {
      server->send(400, "text/plain", "Invalid JSON body");
      return;
    }
    const String rawPath = request["path"] | "";
    if (rawPath.isEmpty()) {
      server->send(400, "text/plain", "Missing path");
      return;
    }
    if (!request["pokemon"].is<JsonObject>()) {
      server->send(400, "text/plain", "Missing pokemon object");
      return;
    }
    if (!PathUtils::isValidSdPath(rawPath)) {
      server->send(400, "text/plain", "Invalid path");
      return;
    }
    const String bookPath = PathUtils::normalizePath(rawPath);
    if (PathUtils::pathContainsProtectedItem(bookPath)) {
      server->send(403, "text/plain", "Cannot access protected items");
      return;
    }
    if (!Storage.exists(bookPath.c_str())) {
      server->send(404, "text/plain", "Book not found");
      return;
    }
    if (!PokemonBookDataStore::supportsBookPath(bookPath.c_str())) {
      server->send(400, "text/plain", "Unsupported book type");
      return;
    }
    if (!PokemonBookDataStore::savePokemonDocument(bookPath.c_str(), request["pokemon"])) {
      server->send(500, "text/plain", "Failed to save pokemon data");
      return;
    }
    JsonDocument response;
    response["ok"] = true;
    response["path"] = bookPath;
    response["pokemon"] = request["pokemon"];
    String json;
    serializeJson(response, json);
    server->send(200, "application/json", json);
  });

  server->on("/api/book-pokemon", HTTP_DELETE, [server] {
    if (!server->hasArg("path")) {
      server->send(400, "text/plain", "Missing path");
      return;
    }
    String bookPath = PathUtils::urlDecode(server->arg("path"));
    if (!PathUtils::isValidSdPath(bookPath)) {
      server->send(400, "text/plain", "Invalid path");
      return;
    }
    bookPath = PathUtils::normalizePath(bookPath);
    if (PathUtils::pathContainsProtectedItem(bookPath)) {
      server->send(403, "text/plain", "Cannot access protected items");
      return;
    }
    if (!Storage.exists(bookPath.c_str())) {
      server->send(404, "text/plain", "Book not found");
      return;
    }
    if (!PokemonBookDataStore::supportsBookPath(bookPath.c_str())) {
      server->send(400, "text/plain", "Unsupported book type");
      return;
    }
    if (!PokemonBookDataStore::deletePokemonDocument(bookPath.c_str())) {
      server->send(500, "text/plain", "Failed to delete pokemon data");
      return;
    }
    JsonDocument response;
    response["ok"] = true;
    response["path"] = bookPath;
    String json;
    serializeJson(response, json);
    server->send(200, "application/json", json);
  });
}
#endif

}  // namespace

void registerFeature() {
#if ENABLE_WEB_POKEDEX_PLUGIN
  core::WebRouteEntry webRouteEntry{};
  webRouteEntry.routeId = "pokemon_party_api";
  webRouteEntry.shouldRegister = shouldRegisterPokemonPartyApiRoute;
  webRouteEntry.mountRoutes = mountPokemonRoutes;
  core::WebRouteRegistry::add(webRouteEntry);
#endif
}

}  // namespace features::pokemon_party
