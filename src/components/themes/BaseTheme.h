#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "components/UITheme.h"

class GfxRenderer;

// Default theme implementation (Classic Theme)
// Additional themes can inherit from this and override methods as needed

class BaseTheme {
 public:
  virtual ~BaseTheme() = default;

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