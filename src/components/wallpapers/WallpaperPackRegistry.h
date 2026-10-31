#pragma once

#include <string>
#include <vector>

// Independent of UI themes. BMP-only v1 collections use the existing Custom
// sleep-screen decoder and can also supply timed refresh without .sleep copies.
struct WallpaperPackChoice {
  std::string id;
  std::string label;
  std::string description;
  std::string author;
  std::string version;
};

class WallpaperPackRegistry {
 public:
  static WallpaperPackRegistry& getInstance();
  void scan();
  // Clear a saved wallpaper ID that no longer exists. Empty means All Wallpapers.
  // Only call after scan(); never discard preferences when SD is unavailable.
  bool reconcileSelection();
  const std::vector<WallpaperPackChoice>& choices() const { return installed; }
  bool selectImage(const char* id, std::string& path) const;
  // Remove only a recognized, validated installed collection.
  bool remove(const std::string& id);

 private:
  std::vector<WallpaperPackChoice> installed;
};
