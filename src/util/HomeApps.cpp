#if CROSSPOINT_DESSEN_APPS
#include "HomeApps.h"

#include <ArduinoJson.h>
#include <HalStorage.h>

#include "PluginLocations.h"

namespace {
constexpr size_t MAX_MANIFEST_SIZE = 8 * 1024;
}

std::vector<HomeApp> scanHomeApps() {
  std::vector<HomeApp> apps;
  for (const PluginLocations::Entry& e : PluginLocations::scanPlugins()) {
    const std::string appBin = e.dir + "/app.bin";
    if (!e.hasManifest || !Storage.exists(appBin.c_str())) continue;

    std::string raw;
    if (!Storage.readFileToString("HOME", e.dir + "/manifest.json", MAX_MANIFEST_SIZE, raw)) continue;
    JsonDocument filter;
    filter["home"] = true;
    filter["title"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) continue;
    if (!(doc["home"].is<bool>() && doc["home"].as<bool>())) continue;

    HomeApp app;
    app.title = doc["title"].is<const char*>() ? doc["title"].as<const char*>() : e.name;
    app.appPath = appBin;
    apps.push_back(std::move(app));
  }
  return apps;
}

#endif  // CROSSPOINT_DESSEN_APPS
