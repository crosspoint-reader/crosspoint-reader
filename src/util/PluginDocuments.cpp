#include "PluginDocuments.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "PluginHttp.h"
#include "PluginLocations.h"
#include "network/ProtectedPaths.h"

namespace plugindocuments {
namespace {
constexpr size_t MAX_MANIFEST_SIZE = 8 * 1024;

struct Walk {
  const std::string& pluginDir;
  bool hasBrowseUrl;
  const char* logTag;
  MenuVisitor visit;
  void* ctx;
  size_t kept = 0;
};

// Returns false once the entry limit is reached, which ends the whole walk.
bool walkLevel(Walk& w, const JsonArrayConst entries, const int16_t parent, const int depth) {
  for (JsonVariantConst e : entries) {
    if (w.kept >= PluginLocations::MAX_MENU_ENTRIES) {
      if (w.logTag) {
        LOG_ERR(w.logTag, "menu over %u entries, rest skipped",
                static_cast<unsigned>(PluginLocations::MAX_MENU_ENTRIES));
      }
      return false;
    }
    MenuItem item;
    item.title = e["title"] | "";
    item.json = e;
    const char* type = e["type"] | "";
    const char* skip = nullptr;  // why the entry is dropped; one log line covers all
    if (!*item.title) {
      skip = "no title";
    } else if (strcmp(type, "menu") == 0) {
      item.type = MenuItem::Type::Menu;
      if (depth >= PluginLocations::MAX_MENU_DEPTH) skip = "too deep";
    } else if (strcmp(type, "catalog") == 0) {
      item.type = MenuItem::Type::Catalog;
      item.target = e["url"] | "";
      if (item.target.empty() && !w.hasBrowseUrl) skip = "no browse url";
    } else if (strcmp(type, "file") == 0) {
      item.type = MenuItem::Type::File;
      item.target = pluginhttp::inPluginDir(w.pluginDir, e["path"] | "");
      if (!protectedpaths::isPluginPath(item.target)) {
        skip = "path rejected";
      } else if (item.target.size() > PluginLocations::MAX_DOCUMENT_PATH) {
        skip = "path too long";
      }
      // Document is the default: a plugin must ask to touch Recents and progress.
      item.document = strcmp(e["mode"] | "document", "book") != 0;
    } else {
      skip = "unknown type";
    }
    if (skip) {
      if (w.logTag) LOG_ERR(w.logTag, "menu entry skipped (%s): '%s' %s", skip, item.title, item.target.c_str());
      continue;
    }
    ++w.kept;
    const int16_t childParent = w.visit(w.ctx, item, parent);
    if (item.type == MenuItem::Type::Menu && !walkLevel(w, e["items"], childParent, depth + 1)) return false;
  }
  return true;
}

int16_t collectDocument(void* ctx, const MenuItem& item, int16_t /*parent*/) {
  if (item.type == MenuItem::Type::File && item.document) {
    static_cast<std::vector<std::string>*>(ctx)->push_back(item.target);
  }
  return -1;
}
}  // namespace

void walkMenu(const JsonArrayConst entries, const std::string& pluginDir, const bool hasBrowseUrl, const char* logTag,
              const MenuVisitor visit, void* ctx) {
  Walk w{pluginDir, hasBrowseUrl, logTag, visit, ctx};
  walkLevel(w, entries, -1, 1);
}

std::vector<std::string> collect() {
  std::vector<std::string> documents;
  // Only the fields walkMenu() reads, so a large device.json parses small.
  JsonDocument filter;
  filter["browse"]["url"] = true;
  JsonObject level = filter["menu"].add<JsonObject>();
  for (int depth = 1; depth <= PluginLocations::MAX_MENU_DEPTH; ++depth) {
    for (const char* key : {"title", "type", "url", "path", "mode"}) level[key] = true;
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
    const char* browseUrl = doc["browse"]["url"] | "";
    walkMenu(doc["menu"], plugin.dir, *browseUrl != '\0', nullptr, collectDocument, &documents);
  }
  return documents;
}

bool isDocument(const void* ctx, const std::string_view fullPath) {
  const auto& documents = *static_cast<const std::vector<std::string>*>(ctx);
  return std::find(documents.begin(), documents.end(), fullPath) != documents.end();
}

}  // namespace plugindocuments
