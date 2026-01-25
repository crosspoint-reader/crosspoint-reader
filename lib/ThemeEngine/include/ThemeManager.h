#pragma once

#include "BasicElements.h"
#include "IniParser.h"
#include "ThemeContext.h"
#include <GfxRenderer.h>
#include <map>
#include <string>
#include <vector>

namespace ThemeEngine {

struct ProcessedAsset {
  std::vector<uint8_t> data;
  int w, h;
  GfxRenderer::Orientation orientation;
};

// Screen render cache - stores full screen state for quick restore
struct ScreenCache {
  uint8_t *buffer = nullptr;
  size_t bufferSize = 0;
  std::string screenName;
  uint32_t contextHash = 0; // Hash of context data to detect changes
  bool valid = false;

  ~ScreenCache() {
    if (buffer) {
      free(buffer);
      buffer = nullptr;
    }
  }

  void invalidate() { valid = false; }
};

class ThemeManager {
private:
  std::map<std::string, UIElement *> elements; // All elements by ID
  std::string currentThemeName;
  int navBookCount = 1;  // Number of navigable book slots (from theme [Global] section)
  std::map<std::string, int> fontMap;

  // Screen-level caching for fast redraw
  std::map<std::string, ScreenCache> screenCaches;
  bool useCaching = true;

  // Track which elements are data-dependent vs static
  std::map<std::string, bool> elementDependsOnData;

  // Factory and property methods
  UIElement *createElement(const std::string &id, const std::string &type);
  void applyProperties(UIElement *elem,
                       const std::map<std::string, std::string> &props);

public:
  static ThemeManager &get() {
    static ThemeManager instance;
    return instance;
  }

  // Initialize defaults (fonts, etc.)
  void begin();

  // Register a font ID mapping (e.g. "UI_12" -> 0)
  void registerFont(const std::string &name, int id);

  // Theme loading
  void loadTheme(const std::string &themeName);
  void unloadTheme();

  // Get current theme name
  const std::string &getCurrentTheme() const { return currentThemeName; }

  // Get number of navigable book slots (from theme config, default 1)
  int getNavBookCount() const { return navBookCount; }

  // Render a screen
  void renderScreen(const std::string &screenName, const GfxRenderer &renderer,
                    const ThemeContext &context);

  // Render with dirty tracking (only redraws changed regions)
  void renderScreenOptimized(const std::string &screenName,
                             const GfxRenderer &renderer,
                             const ThemeContext &context,
                             const ThemeContext *prevContext = nullptr);

  // Invalidate all caches (call when theme changes or screen switches)
  void invalidateAllCaches();

  // Invalidate specific screen cache
  void invalidateScreenCache(const std::string &screenName);

  // Enable/disable caching
  void setCachingEnabled(bool enabled) { useCaching = enabled; }
  bool isCachingEnabled() const { return useCaching; }

  // Asset path resolution
  std::string getAssetPath(const std::string &assetName);

  // Asset caching
  const std::vector<uint8_t> *getCachedAsset(const std::string &path);
  const ProcessedAsset *getProcessedAsset(const std::string &path,
                                          GfxRenderer::Orientation orientation,
                                          int targetW = 0, int targetH = 0);
  void cacheProcessedAsset(const std::string &path,
                           const ProcessedAsset &asset,
                           int targetW = 0, int targetH = 0);

  // Clear asset caches (for memory management)
  void clearAssetCaches();

  // Get element by ID (useful for direct manipulation)
  UIElement *getElement(const std::string &id) {
    auto it = elements.find(id);
    return it != elements.end() ? it->second : nullptr;
  }

private:
  std::map<std::string, std::vector<uint8_t>> assetCache;
  std::map<std::string, ProcessedAsset> processedCache;

  // Compute a simple hash of context data for cache invalidation
  uint32_t computeContextHash(const ThemeContext &context,
                              const std::string &screenName);
};

} // namespace ThemeEngine
