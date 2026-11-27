#pragma once
#if CROSSPOINT_DESSEN_APPS

#include <string>
#include <vector>

// A dessen app whose plugin manifest.json declares "home": true and that ships
// an app.bin — i.e. one that can be selected as the device's home screen in
// Settings. Scans the plugin roots; cheap enough to call when the picker opens.
struct HomeApp {
  std::string title;    // manifest "title" (falls back to folder name)
  std::string appPath;  // absolute path of its app.bin (what SETTINGS.customHomeApp stores)
};

std::vector<HomeApp> scanHomeApps();

#endif  // CROSSPOINT_DESSEN_APPS
