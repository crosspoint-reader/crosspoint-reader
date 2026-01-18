#pragma once

#include <GfxRenderer.h>

#include "CrossPointSettings.h"

struct Rect {
  int x;
  int y;
  int width;
  int height;

  // Constructor for explicit initialization
  explicit Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};

class ITheme {
 public:
  virtual ~ITheme() = default;

  // Property getters
  virtual Rect getWindowContentFrame(GfxRenderer& renderer);

  // Component drawing methods
  virtual void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total);
  virtual void drawBattery(const GfxRenderer& renderer, Rect rect, bool showPercentage = true);
  virtual void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                        const std::function<std::string(int index)>& rowTitle, bool hasIcon,
                        const std::function<std::string(int index)>& rowIcon, bool hasValue,
                        const std::function<std::string(int index)>& rowValue);

  virtual void drawWindowFrame(GfxRenderer& renderer, Rect rect, bool isPopup, const char* title);
  virtual void drawFullscreenWindowFrame(GfxRenderer& renderer, const char* title);
};

class UITheme {
 public:
  static void initialize();
  static void setTheme(CrossPointSettings::UI_THEME type);

  static Rect getWindowContentFrame(GfxRenderer& renderer);

  static void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total);
  static void drawBattery(const GfxRenderer& renderer, Rect rect, bool showPercentage = true);
  static void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                       const std::function<std::string(int index)>& rowTitle, bool hasIcon,
                       const std::function<std::string(int index)>& rowIcon, bool hasValue,
                       const std::function<std::string(int index)>& rowValue);

  static void drawWindowFrame(GfxRenderer& renderer, Rect rect, bool isPopup, const char* title);
  static void drawFullscreenWindowFrame(GfxRenderer& renderer, const char* title);
};