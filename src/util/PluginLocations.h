#pragma once
#include <string>
#include <vector>

// Plugin folders may live under any of these SD roots; earlier roots win on
// name collisions. /.crosspoint/plugins is the historical home; /plugins and
// /.plugins are friendlier for users copying folders onto the card from a
// computer.
namespace PluginLocations {
inline constexpr const char* ROOTS[] = {"/.crosspoint/plugins", "/plugins", "/.plugins"};
inline constexpr size_t ROOT_COUNT = sizeof(ROOTS) / sizeof(ROOTS[0]);

enum class DeviceKind { None, Catalog, Background };
enum class PickerAction { None, Catalog, Readme };

constexpr DeviceKind classifyDeviceManifest(const bool hasBrowseUrl, const bool hasEvents) {
  if (hasBrowseUrl) return DeviceKind::Catalog;
  if (hasEvents) return DeviceKind::Background;
  return DeviceKind::None;
}

// A catalog opens; any other plugin with a README shows it (web-only plugins
// included, so their setup notes are readable on the device).
constexpr PickerAction pickerAction(const DeviceKind kind, const bool hasReadme) {
  if (kind == DeviceKind::Catalog) return PickerAction::Catalog;
  return hasReadme ? PickerAction::Readme : PickerAction::None;
}

// One SD plugin folder, classified by the marker files it carries.
struct Entry {
  std::string name;          // folder name
  std::string dir;           // "<root>/<name>"
  bool hasPluginJs = false;  // browser-side plugin (plugin.js)
  bool hasDevice = false;    // on-device catalog manifest (device.json)
  bool hasManifest = false;  // web UI card metadata (manifest.json)
};

// Scans every root. The earliest root containing a folder name claims it —
// matching findPluginDir, which serves that folder's files — and folders with
// none of the marker files are omitted. This is the single definition of
// "what is a plugin"; callers filter by the markers they need.
std::vector<Entry> scanPlugins();

// Directory of the named plugin ("<root>/<name>"), or "" when absent.
std::string findPluginDir(const char* name);
}  // namespace PluginLocations
