#pragma once
#include <string>
#include <string_view>
#include <vector>

// Pages a plugin menu opens as documents (device.json "menu" file entries
// without "mode": "book"). The plugin owns and rewrites them, so the Library
// leaves them out; a plugin that wants a file on the shelf marks it "book".
namespace plugindocuments {

// Full SD paths across every installed plugin, read from device.json.
std::vector<std::string> collect();

// library::SkipFile callback; ctx is the vector collect() returned.
bool isDocument(const void* ctx, std::string_view fullPath);

}  // namespace plugindocuments
