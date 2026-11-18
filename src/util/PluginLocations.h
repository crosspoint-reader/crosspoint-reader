#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Plugin folders may live under any of these SD roots; earlier roots win on
// name collisions. /.crosspoint/plugins is the historical home; /plugins and
// /.plugins are friendlier for users copying folders onto the card from a
// computer.
namespace PluginLocations {
inline constexpr const char* ROOTS[] = {"/.crosspoint/plugins", "/plugins", "/.plugins"};
inline constexpr size_t ROOT_COUNT = sizeof(ROOTS) / sizeof(ROOTS[0]);
inline constexpr const char* INSTALL_MARKER = ".installing";

inline bool isPluginRoot(const std::string& dir) {
  for (size_t i = 0; i < ROOT_COUNT; i++) {
    if (dir == ROOTS[i]) return true;
  }
  return false;
}

// A bundle targeting one direct child of a plugin root may update an existing
// same-named plugin in place instead of creating a duplicate in another root.
// Nested and non-plugin bundle destinations keep their configured behavior.
inline bool shouldReuseInstalledBundleDir(const std::string& destDir, const std::string& subdir) {
  return !subdir.empty() && subdir.find('/') == std::string::npos && isPluginRoot(destDir);
}

enum class DeviceKind { None, Catalog, Background };
enum class PickerAction { None, Catalog, Readme };

constexpr bool isDiscoverableEntry(const bool quarantined, const bool hasPluginJs, const bool hasDevice,
                                   const bool hasManifest) {
  return !quarantined && (hasPluginJs || hasDevice || hasManifest);
}

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

// Exactly MAJOR.MINOR.PATCH: three digit-only components, nothing else (no
// sign, spaces, suffix or fourth part), each fitting in 32 bits.
inline bool parseVersion(const std::string& v, uint32_t (&out)[3]) {
  size_t pos = 0;
  for (int k = 0; k < 3; k++) {
    if (k > 0 && (pos >= v.size() || v[pos++] != '.')) return false;
    if (pos >= v.size() || v[pos] < '0' || v[pos] > '9') return false;
    uint64_t n = 0;
    for (; pos < v.size() && v[pos] >= '0' && v[pos] <= '9'; pos++) {
      n = n * 10 + static_cast<uint64_t>(v[pos] - '0');
      if (n > UINT32_MAX) return false;
    }
    out[k] = static_cast<uint32_t>(n);
  }
  return pos == v.size();
}

// Plugin versions are MAJOR.MINOR.PATCH (the catalog contract). True when
// `catalog` is newer than `installed`. An installed copy without a valid
// version is offered the update; a malformed catalog version never is.
inline bool isNewerVersion(const std::string& catalog, const std::string& installed) {
  uint32_t c[3], i[3];
  if (!parseVersion(catalog, c)) return false;
  if (!parseVersion(installed, i)) return true;
  for (int k = 0; k < 3; k++) {
    if (c[k] != i[k]) return c[k] > i[k];
  }
  return false;
}

// One SD plugin folder, classified by the marker files it carries.
struct Entry {
  std::string name;          // folder name
  std::string dir;           // "<root>/<name>"
  bool hasPluginJs = false;  // browser-side plugin (plugin.js)
  bool hasDevice = false;    // on-device catalog manifest (device.json)
  bool hasManifest = false;  // web UI card metadata (manifest.json)
};

// Returns installed plugins only while the global Plugin System is enabled.
// The physical scan still uses root priority and direct child folders only.
std::vector<Entry> scanPlugins();

// Raw on-disk probes used only for migration/bootstrap UI. These ignore the
// global Plugin System switch and installation quarantine so recovery can still
// detect pre-existing or interrupted plugin directories.
std::vector<Entry> scanPluginsOnDisk();
bool anyPluginInstalledOnDisk();
std::string findPluginDirOnDisk(const char* name);
bool isPluginQuarantined(const std::string& dir);

// Directory of the named plugin ("<root>/<name>"), or "" when absent,
// globally disabled, or quarantined by an in-progress installation.
std::string findPluginDir(const char* name);
}  // namespace PluginLocations
