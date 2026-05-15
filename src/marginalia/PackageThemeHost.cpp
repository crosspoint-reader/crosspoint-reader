#include "PackageThemeHost.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include "PackageStore.h"

namespace Marginalia {

namespace {

constexpr size_t MAX_THEME_BYTES = 2048;
constexpr const char* THEME_DESCRIPTOR = "src/theme.json";

bool loaded = false;
bool invertDisplay = false;

std::string packageThemePath(const PackageManifest& package) {
  return std::string(PACKAGE_ROOT) + "/" + package.id + "/" + THEME_DESCRIPTOR;
}

bool themeDescriptorEnablesInvert(const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead("MPKG", path.c_str(), file)) {
    return false;
  }

  const size_t size = file.size();
  file.close();
  if (size == 0 || size > MAX_THEME_BYTES) {
    LOG_ERR("MPKG", "Theme descriptor has invalid size: %s", path.c_str());
    return false;
  }

  const String json = Storage.readFile(path.c_str());
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("MPKG", "Theme descriptor parse failed: %s", path.c_str());
    return false;
  }

  return (doc["schemaVersion"] | 0) == 1 && doc["scope"].is<const char*>() && doc["mode"].is<const char*>() &&
         std::string(doc["scope"].as<const char*>()) == "os" &&
         std::string(doc["mode"].as<const char*>()) == "invert-screen";
}

void reloadThemeState() {
  invertDisplay = false;

  PackageStore store;
  store.scan();
  for (const auto& package : store.packages()) {
    if (!package.enabled || !package.compatible || package.kind != "theme") {
      continue;
    }

    const std::string path = packageThemePath(package);
    if (themeDescriptorEnablesInvert(path) && readPackageSettingBool(package.id, "invertScreen", true)) {
      invertDisplay = true;
      LOG_DBG("MPKG", "Enabled OS invert theme from package: %s", package.id.c_str());
      break;
    }
  }

  loaded = true;
}

}  // namespace

bool packageThemeInvertsDisplay() {
  if (!loaded) {
    reloadThemeState();
  }
  return invertDisplay;
}

void markPackageThemeHostDirty() { loaded = false; }

}  // namespace Marginalia
