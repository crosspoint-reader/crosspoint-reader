#pragma once

#include <functional>
#include <memory>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"

enum class ContentHints : uint8_t {
  NONE = 0,
  BOTTOM_HINTS = 1 << 0,
  SIDE_HINTS = 1 << 1,
};

constexpr ContentHints operator|(ContentHints a, ContentHints b) noexcept {
  return static_cast<ContentHints>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

constexpr ContentHints operator&(ContentHints a, ContentHints b) noexcept {
  return static_cast<ContentHints>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() const { return *currentMetrics; }
  const BaseTheme& getTheme() const { return *currentTheme; }
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle);
  // Returns the drawable content Rect accounting for screen orientation and visible button hints.
  // Bottom hints occupy the physical bottom edge; side hints occupy the physical right edge.
  // The mapping to logical edges is orientation-dependent.
  static Rect getContentRect(const GfxRenderer& renderer, ContentHints hints);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarHeight();
  static int getProgressBarHeight();

 private:
  const ThemeMetrics* currentMetrics;
  std::unique_ptr<BaseTheme> currentTheme;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
