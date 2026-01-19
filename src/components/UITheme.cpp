#include "UITheme.h"

#include <GfxRenderer.h>

#include <memory>

#include "components/themes/BaseTheme.h"
#include "components/themes/rounded/RoundedTheme.h"

std::unique_ptr<BaseTheme> currentTheme = nullptr;

// Initialize theme based on settings
void UITheme::initialize() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  switch (type) {
    case CrossPointSettings::UI_THEME::CLASSIC:
      Serial.printf("[%lu] [UI] Using Classic theme\n", millis());
      currentTheme = std::unique_ptr<BaseTheme>(new BaseTheme());
      break;
    case CrossPointSettings::UI_THEME::ROUNDED:
      Serial.printf("[%lu] [UI] Using Rounded theme\n", millis());
      currentTheme = std::unique_ptr<BaseTheme>(new RoundedTheme());
      break;
  }
}

// Forward all component methods to the current theme
void UITheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) {
  if (currentTheme != nullptr) {
    currentTheme->drawProgressBar(renderer, rect, current, total);
  }
}

void UITheme::drawBattery(const GfxRenderer& renderer, Rect rect, bool showPercentage) {
  if (currentTheme != nullptr) {
    currentTheme->drawBattery(renderer, rect, showPercentage);
  }
}

void UITheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                       const std::function<std::string(int index)>& rowTitle, bool hasIcon,
                       const std::function<std::string(int index)>& rowIcon, bool hasValue,
                       const std::function<std::string(int index)>& rowValue) {
  if (currentTheme != nullptr) {
    currentTheme->drawList(renderer, rect, itemCount, selectedIndex, rowTitle, hasIcon, rowIcon, hasValue, rowValue);
  }
}

void UITheme::drawWindowFrame(GfxRenderer& renderer, Rect rect, bool isPopup, const char* title) {
  if (currentTheme != nullptr) {
    currentTheme->drawWindowFrame(renderer, rect, isPopup, title);
  }
}

void UITheme::drawFullscreenWindowFrame(GfxRenderer& renderer, const char* title) {
  if (currentTheme != nullptr) {
    currentTheme->drawFullscreenWindowFrame(renderer, title);
  }
}

Rect UITheme::getWindowContentFrame(GfxRenderer& renderer) {
  if (currentTheme != nullptr) {
    return currentTheme->getWindowContentFrame(renderer);
  }
  return Rect{};
}
