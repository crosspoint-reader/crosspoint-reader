#include "PluginDocuments.h"

#include <ArduinoJson.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstring>

#include "PluginHttp.h"
#include "PluginLocations.h"
#include "network/ProtectedPaths.h"

namespace plugindocuments {
namespace {
constexpr size_t MAX_MANIFEST_SIZE = 8 * 1024;

// Mirrors PluginCatalogActivity::parseMenu's acceptance rules, so the shelf hides
// the files the menu would open as documents.
void collectMenu(const JsonArrayConst entries, const std::string& pluginDir, const int depth, size_t& seen,
                 std::vector<std::string>& out) {
  for (JsonVariantConst e : entries) {
    if (seen >= PluginLocations::MAX_MENU_ENTRIES) return;
    if (!e["title"].is<const char*>() || !*e["title"].as<const char*>()) continue;
    const char* type = e["type"] | "";
    if (strcmp(type, "menu") == 0) {
      if (depth >= PluginLocations::MAX_MENU_DEPTH) continue;
      ++seen;
      collectMenu(e["items"], pluginDir, depth + 1, seen, out);
      continue;
    }
    if (strcmp(type, "catalog") == 0) {
      ++seen;
      continue;
    }
    if (strcmp(type, "file") != 0) continue;
    std::string path = pluginhttp::inPluginDir(pluginDir, e["path"] | "");
    if (!protectedpaths::isPluginPath(path)) continue;
    ++seen;
    if (strcmp(e["mode"] | "document", "book") != 0) out.push_back(std::move(path));
  }
}
}  // namespace

std::vector<std::string> collect() {
  std::vector<std::string> documents;
  JsonDocument filter;
  JsonObject level = filter["menu"].add<JsonObject>();
  for (int depth = 1; depth <= PluginLocations::MAX_MENU_DEPTH; ++depth) {
    level["title"] = true;
    level["type"] = true;
    level["path"] = true;
    level["mode"] = true;
    if (depth < PluginLocations::MAX_MENU_DEPTH) level = level["items"].add<JsonObject>();
  }
  // On disk, not gated by the Plugins setting: a plugin's page is not a book
  // whether or not the plugin system is switched on.
  for (const auto& plugin : PluginLocations::scanPluginsOnDisk()) {
    if (!plugin.hasDevice) continue;
    std::string raw;
    if (!Storage.readFileToString("PDOC", plugin.dir + "/device.json", MAX_MANIFEST_SIZE, raw)) continue;
    JsonDocument doc;
    if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) continue;
    size_t seen = 0;
    collectMenu(doc["menu"], plugin.dir, 1, seen, documents);
  }
  return documents;
}

bool isDocument(const void* ctx, const std::string_view fullPath) {
  const auto& documents = *static_cast<const std::vector<std::string>*>(ctx);
  return std::find(documents.begin(), documents.end(), fullPath) != documents.end();
}

}  // namespace plugindocuments
