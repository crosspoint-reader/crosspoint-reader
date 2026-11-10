#pragma once
#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Pages a plugin menu opens as documents (device.json "menu" file entries
// without "mode": "book"). The plugin owns and rewrites them, so the Library
// leaves them out; a plugin that wants a file on the shelf marks it "book".
namespace plugindocuments {

// One device.json "menu" entry that the menu keeps.
struct MenuItem {
  enum class Type : uint8_t { Menu, Catalog, File };
  Type type = Type::Menu;
  const char* title = "";
  std::string target;     // File: absolute SD path. Catalog: url override ("" = browse.url)
  bool document = false;  // File: opens as a plugin document (mode other than "book")
  JsonVariantConst json;  // the entry itself, for type-specific fields
};

// Returns the `parent` passed to the entry's own items (submenus only).
using MenuVisitor = int16_t (*)(void* ctx, const MenuItem& item, int16_t parent);

// Visits, in order, every menu entry the device keeps: the single place for the
// menu's limits and skip rules, so the menu and the Library always agree.
// logTag: log each skipped entry under this tag; nullptr stays quiet.
void walkMenu(JsonArrayConst entries, const std::string& pluginDir, bool hasBrowseUrl, const char* logTag,
              MenuVisitor visit, void* ctx);

// Full SD paths across every installed plugin, read from device.json.
std::vector<std::string> collect();

// True when replacing this file can change what collect() returns (a device.json).
bool changesDocuments(std::string_view fullPath);

// library::SkipFile callback; ctx is the vector collect() returned.
bool isDocument(const void* ctx, std::string_view fullPath);

}  // namespace plugindocuments
