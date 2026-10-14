#include "PluginLocations.h"

#include <HalStorage.h>

#include <algorithm>

namespace PluginLocations {

std::vector<Entry> scanPlugins() {
  std::vector<Entry> plugins;
  plugins.reserve(8);
  std::vector<std::string> seen;
  seen.reserve(8);
  for (size_t r = 0; r < ROOT_COUNT; r++) {
    HalFile root = Storage.open(ROOTS[r]);
    if (!root || !root.isDirectory()) continue;
    for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
      if (!entry.isDirectory()) continue;
      char name[128];
      if (entry.getName(name, sizeof(name)) == 0 || name[0] == '.') continue;
      if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
      // Claimed even without markers: findPluginDir resolves this name here,
      // so a same-named folder in a later root must not be reported instead.
      seen.emplace_back(name);

      Entry e;
      e.name = name;
      e.dir = std::string(ROOTS[r]) + "/" + name;
      e.hasPluginJs = Storage.exists((e.dir + "/plugin.js").c_str());
      e.hasDevice = Storage.exists((e.dir + "/device.json").c_str());
      e.hasManifest = Storage.exists((e.dir + "/manifest.json").c_str());
      if (e.hasPluginJs || e.hasDevice || e.hasManifest) plugins.push_back(std::move(e));
    }
  }
  return plugins;
}

std::string findPluginDir(const char* name) {
  for (size_t i = 0; i < ROOT_COUNT; i++) {
    std::string dir = std::string(ROOTS[i]) + "/" + name;
    if (Storage.exists(dir.c_str())) return dir;
  }
  return {};
}

}  // namespace PluginLocations
