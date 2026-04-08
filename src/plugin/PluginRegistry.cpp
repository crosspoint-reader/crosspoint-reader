#include "PluginRegistry.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

// ---- Plugin table (explicit array — fork authors add entries here) ----------

// Forward-declare plugin descriptors from src/plugins/
extern const CprPlugin helloPlugin;
extern const CprPlugin hardcoverPlugin;

static const CprPlugin* const pluginTable[] = {
    &helloPlugin,
    &hardcoverPlugin,
};

static constexpr int pluginCount = sizeof(pluginTable) / sizeof(pluginTable[0]);
static_assert(pluginCount <= PluginRegistry::MAX_PLUGINS, "Too many plugins registered");

// ---- Per-plugin runtime state -----------------------------------------------

namespace {
constexpr char PLUGINS_JSON_PATH[] = "/.crosspoint/plugins.json";
constexpr size_t MAX_PLUGIN_JSON_SIZE = 4096;  // Reasonable limit for plugin state JSON

struct PluginState {
  bool enabled = false;
  bool compatible = true;
};

PluginState pluginStates[PluginRegistry::MAX_PLUGINS] = {};

// ---- Semver helpers ---------------------------------------------------------

struct Semver {
  int major = 0;
  int minor = 0;
  int patch = 0;
};

/// Parse "major.minor.patch" from a version string.  Ignores pre-release /
/// build suffixes after the three numeric components.
bool parseSemver(const char* str, Semver& out) {
  if (!str) return false;
  // sscanf with %d safely stops at the first non-digit after each component.
  return sscanf(str, "%d.%d.%d", &out.major, &out.minor, &out.patch) == 3;
}

/// Return true when `required` <= `current` within the same major version.
bool semverSatisfied(const Semver& required, const Semver& current) {
  if (current.major != required.major) return false;
  if (current.minor != required.minor) return current.minor > required.minor;
  return current.patch >= required.patch;
}
}  // namespace

// ---- Public API -------------------------------------------------------------

void PluginRegistry::init() {
  // 1. Parse firmware version once
  Semver firmwareVer{};
  if (!parseSemver(CROSSPOINT_VERSION, firmwareVer)) {
    LOG_ERR("PLG", "Cannot parse firmware version: %s", CROSSPOINT_VERSION);
  }

  // 2. Compatibility check for every registered plugin
  for (int i = 0; i < pluginCount; i++) {
    Semver required{};
    if (parseSemver(pluginTable[i]->minCpr, required)) {
      pluginStates[i].compatible = semverSatisfied(required, firmwareVer);
    } else {
      LOG_ERR("PLG", "Bad min_cpr in plugin '%s'", pluginTable[i]->id);
      pluginStates[i].compatible = false;
    }

    if (!pluginStates[i].compatible) {
      pluginStates[i].enabled = false;
      LOG_INF("PLG", "Plugin '%s' incompatible (needs %s, have %s)", pluginTable[i]->id, pluginTable[i]->minCpr,
              CROSSPOINT_VERSION);
    }
  }

  // 3. Load enabled state from SD card
  if (Storage.exists(PLUGINS_JSON_PATH)) {
    HalFile file;
    if (Storage.openFileForRead("PLG", PLUGINS_JSON_PATH, file)) {
      size_t size = file.size();
      file.close();
      if (size > MAX_PLUGIN_JSON_SIZE) {
        LOG_ERR("PLG", "plugins.json too large (%zu bytes), skipping", size);
      } else {
        String json = Storage.readFile(PLUGINS_JSON_PATH);
        if (!json.isEmpty()) {
          JsonDocument doc;
          auto err = deserializeJson(doc, json);
          if (err) {
            LOG_ERR("PLG", "plugins.json parse error: %s", err.c_str());
          } else {
            for (int i = 0; i < pluginCount; i++) {
              if (doc[pluginTable[i]->id].is<bool>()) {
                pluginStates[i].enabled = doc[pluginTable[i]->id].as<bool>();
              }
              // Override: incompatible plugins are always disabled
              if (!pluginStates[i].compatible) {
                pluginStates[i].enabled = false;
              }
            }
          }
        }
      }
    }
  }

  LOG_INF("PLG", "Plugin registry initialized: %d plugin(s)", pluginCount);
}

bool PluginRegistry::isEnabled(const char* id) {
  for (int i = 0; i < pluginCount; i++) {
    if (strcmp(pluginTable[i]->id, id) == 0) return pluginStates[i].enabled;
  }
  return false;
}

void PluginRegistry::setEnabled(const char* id, bool enabled) {
  for (int i = 0; i < pluginCount; i++) {
    if (strcmp(pluginTable[i]->id, id) == 0) {
      if (!pluginStates[i].compatible) return;  // cannot enable incompatible plugin
      pluginStates[i].enabled = enabled;
      saveState();
      return;
    }
  }
}

void PluginRegistry::saveState() {
  if (!Storage.mkdir("/.crosspoint")) {
    LOG_ERR("PLG", "Failed to create .crosspoint directory");
    return;
  }

  JsonDocument doc;
  for (int i = 0; i < pluginCount; i++) {
    doc[pluginTable[i]->id] = pluginStates[i].enabled;
  }

  String json;
  json.reserve(256);  // Pre-allocate to avoid reallocation for typical plugin counts
  size_t len = serializeJson(doc, json);
  if (len == 0) {
    LOG_ERR("PLG", "plugins.json serialize failed");
    return;
  }
  if (!Storage.writeFile(PLUGINS_JSON_PATH, json)) {
    LOG_ERR("PLG", "Failed to write plugins.json");
  }
}

void PluginRegistry::dispatchBoot() {
  for (int i = 0; i < pluginCount; i++) {
    if (pluginStates[i].enabled && pluginStates[i].compatible && pluginTable[i]->onBoot) {
      pluginTable[i]->onBoot();
    }
  }
}

void PluginRegistry::dispatchBookOpen(const char* path) {
  for (int i = 0; i < pluginCount; i++) {
    if (pluginStates[i].enabled && pluginStates[i].compatible && pluginTable[i]->onBookOpen) {
      pluginTable[i]->onBookOpen(path);
    }
  }
}

void PluginRegistry::dispatchBookClose() {
  for (int i = 0; i < pluginCount; i++) {
    if (pluginStates[i].enabled && pluginStates[i].compatible && pluginTable[i]->onBookClose) {
      pluginTable[i]->onBookClose();
    }
  }
}

void PluginRegistry::dispatchPageTurn(int chapter, int page) {
  for (int i = 0; i < pluginCount; i++) {
    if (pluginStates[i].enabled && pluginStates[i].compatible && pluginTable[i]->onPageTurn) {
      pluginTable[i]->onPageTurn(chapter, page);
    }
  }
}

void PluginRegistry::dispatchSleep() {
  for (int i = 0; i < pluginCount; i++) {
    if (pluginStates[i].enabled && pluginStates[i].compatible && pluginTable[i]->onSleep) {
      pluginTable[i]->onSleep();
    }
  }
}

void PluginRegistry::dispatchWake() {
  for (int i = 0; i < pluginCount; i++) {
    if (pluginStates[i].enabled && pluginStates[i].compatible && pluginTable[i]->onWake) {
      pluginTable[i]->onWake();
    }
  }
}

int PluginRegistry::count() { return pluginCount; }

const CprPlugin* PluginRegistry::get(int index) {
  if (index < 0 || index >= pluginCount) return nullptr;
  return pluginTable[index];
}

bool PluginRegistry::isCompatible(int index) {
  if (index < 0 || index >= pluginCount) return false;
  return pluginStates[index].compatible;
}
