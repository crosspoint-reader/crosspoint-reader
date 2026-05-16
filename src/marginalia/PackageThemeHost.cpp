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
bool halfRefresh = false;
bool disableTextAntialiasing = false;

std::string packageThemePath(const PackageManifest& package) {
  return std::string(PACKAGE_ROOT) + "/" + package.id + "/" + THEME_DESCRIPTOR;
}

struct ThemeDescriptor {
  bool invertDisplay = false;
  bool halfRefresh = false;
  bool disableTextAntialiasing = false;
  bool packageTextAntialiasingSetting = false;
};

ThemeDescriptor readThemeDescriptor(const std::string& path) {
  ThemeDescriptor descriptor;
  FsFile file;
  if (!Storage.openFileForRead("MPKG", path.c_str(), file)) {
    return descriptor;
  }

  const size_t size = file.size();
  file.close();
  if (size == 0 || size > MAX_THEME_BYTES) {
    LOG_ERR("MPKG", "Theme descriptor has invalid size: %s", path.c_str());
    return descriptor;
  }

  const String json = Storage.readFile(path.c_str());
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("MPKG", "Theme descriptor parse failed: %s", path.c_str());
    return descriptor;
  }

  if ((doc["schemaVersion"] | 0) != 1 || !doc["scope"].is<const char*>() || !doc["mode"].is<const char*>()) {
    return descriptor;
  }
  if (std::string(doc["scope"].as<const char*>()) != "os") {
    return descriptor;
  }
  if (std::string(doc["mode"].as<const char*>()) == "invert-screen") {
    descriptor.invertDisplay = true;
  }
  if (doc["refreshMode"].is<const char*>() && std::string(doc["refreshMode"].as<const char*>()) == "half") {
    descriptor.halfRefresh = true;
  }
  if (doc["textAntialiasing"].is<const char*>() && std::string(doc["textAntialiasing"].as<const char*>()) == "off") {
    descriptor.disableTextAntialiasing = true;
  }
  if (doc["textAntialiasing"].is<const char*>() &&
      std::string(doc["textAntialiasing"].as<const char*>()) == "package-setting") {
    descriptor.packageTextAntialiasingSetting = true;
  }
  return descriptor;
}

void reloadThemeState() {
  invertDisplay = false;
  halfRefresh = false;
  disableTextAntialiasing = false;

  PackageStore store;
  store.scan();
  for (const auto& package : store.packages()) {
    if (!package.enabled || !package.compatible || package.kind != "theme") {
      continue;
    }

    const std::string path = packageThemePath(package);
    const ThemeDescriptor descriptor = readThemeDescriptor(path);
    if (descriptor.invertDisplay && readPackageSettingBool(package.id, "invertScreen", true)) {
      invertDisplay = true;
      halfRefresh = halfRefresh || descriptor.halfRefresh;
      disableTextAntialiasing = disableTextAntialiasing || descriptor.disableTextAntialiasing;
      if (descriptor.packageTextAntialiasingSetting) {
        disableTextAntialiasing =
            disableTextAntialiasing || !readPackageSettingBool(package.id, "textAntialiasing", false);
      }
      LOG_DBG("MPKG", "Enabled OS invert theme from package: %s", package.id.c_str());
    }
  }

  loaded = true;
}

}  // namespace

bool packageThemeInvertsDisplay() { return loaded && invertDisplay; }

bool packageThemeRequestsHalfRefresh() { return loaded && halfRefresh; }

bool packageThemeDisablesTextAntialiasing() { return loaded && disableTextAntialiasing; }

void refreshPackageThemeHost() { reloadThemeState(); }

void markPackageThemeHostDirty() { loaded = false; }

}  // namespace Marginalia
