#include "PluginLocations.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace PluginLocations {

std::vector<Entry> scanPlugins() {
  std::vector<Entry> plugins;
  plugins.reserve(8);
  std::vector<std::string> seen;
  seen.reserve(8);

  for (size_t r = 0; r < kRootCount; r++) {
    HalFile root = Storage.open(kRoots[r]);
    if (!root || !root.isDirectory()) continue;

    size_t examined = 0;
    for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
      if (!entry.isDirectory()) continue;
      if (++examined > kMaxDirsPerRoot) {
        LOG_ERR("PLG", "%s holds more than %u directories; ignoring the rest", kRoots[r],
                static_cast<unsigned>(kMaxDirsPerRoot));
        break;
      }
      char name[128];
      if (entry.getName(name, sizeof(name)) == 0 || name[0] == '.') continue;
      // The Plugins page's staging folder for an install in progress, or one a cut install left.
      const size_t nameLength = strlen(name);
      if (nameLength > 4 && strcmp(name + nameLength - 4, ".new") == 0) continue;
      if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
      // Claimed even when it carries no marker: findPluginDir resolves this name
      // to this root, so a same-named folder in a later root must not be
      // reported in its place.
      seen.emplace_back(name);

      Entry e;
      e.name = name;
      e.dir = std::string(kRoots[r]) + "/" + name;
      e.hasPluginJs = Storage.exists((e.dir + "/plugin.js").c_str());
      e.hasManifest = Storage.exists((e.dir + "/manifest.json").c_str());
      e.disabled = Storage.exists((e.dir + "/disabled").c_str());
      e.root = kRoots[r];
      if (e.hasPluginJs || e.hasManifest) plugins.push_back(std::move(e));
    }
  }
  return plugins;
}

std::string findPluginDir(const char* name) {
  for (size_t i = 0; i < kRootCount; i++) {
    std::string dir = std::string(kRoots[i]) + "/" + name;
    if (Storage.exists(dir.c_str())) return dir;
  }
  return {};
}

}  // namespace PluginLocations
