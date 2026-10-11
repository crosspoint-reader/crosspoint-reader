#pragma once
#include <string>
#include <vector>

// Ported from crosspoint-reader PR #2734 ("feat: Add browser-side plugin system
// with SD card support", Justin Mitchell / @itsthisjustin), whose design this
// follows closely: the three SD roots, earliest-root-wins collision handling,
// and the marker-file classification are all his. Reduced here to the browser
// plugin surface only - none of that branch's device-capability endpoints,
// on-device catalog or content-protection work is carried over - and bounded
// with a per-root directory cap.
//
// Where web-UI plugins live on the SD card. A plugin is just a folder holding a
// plugin.js (and optionally a manifest.json); the web server discovers it and
// the browser loads it, so adding one needs no firmware build.
//
// /.crosspoint/plugins is the canonical home. /plugins and /.plugins are also
// scanned because a dot-prefixed folder is awkward to create on some desktop
// file managers, and copying a folder onto the card should be all it takes.
namespace PluginLocations {
inline constexpr const char* kRoots[] = {"/.crosspoint/plugins", "/plugins", "/.plugins"};
inline constexpr size_t kRootCount = sizeof(kRoots) / sizeof(kRoots[0]);
// Where the Plugins page installs: the canonical root. Being inside a dot folder, the web file
// endpoints leave it alone unless Show Hidden Files is on, so a plugin there normally changes only
// through that page.
inline constexpr const char* kInstallRoot = kRoots[0];

// Directories examined per root before the scan gives up. Each candidate costs
// two SD stats to classify, and the scan runs inside a single request, so an
// unbounded loop over a folder that happens to hold hundreds of subdirectories
// could outlast the task watchdog. Far above any real plugin count.
inline constexpr size_t kMaxDirsPerRoot = 64;

// One plugin folder, classified by the marker files it carries.
struct Entry {
  std::string name;          // folder name
  std::string dir;           // "<root>/<name>"
  bool hasPluginJs = false;  // browser-side plugin (plugin.js)
  bool hasManifest = false;  // optional card metadata (manifest.json)
  bool disabled = false;     // a "disabled" marker file: listed, but the pages do not load it
  const char* root = "";     // the kRoots entry it was found in
};

// Scans every root. The earliest root holding a folder name claims it - matching
// findPluginDir, which serves that folder's files - and folders carrying no
// marker file are omitted. Callers filter by the markers they need.
std::vector<Entry> scanPlugins();

// Directory of the named plugin ("<root>/<name>"), or "" when absent. The caller
// must have validated `name` as a single path component.
std::string findPluginDir(const char* name);
}  // namespace PluginLocations
