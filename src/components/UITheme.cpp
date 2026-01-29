#include "UITheme.h"

#include <GfxRenderer.h>

#include <memory>

#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/lyra/LyraTheme.h"

std::unique_ptr<BaseTheme> currentTheme = nullptr;
const ThemeMetrics* UITheme::currentMetrics = &BaseMetrics::values;

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
      currentMetrics = &BaseMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA:
      Serial.printf("[%lu] [UI] Using Lyra theme\n", millis());
      currentTheme = std::unique_ptr<BaseTheme>(new LyraTheme());
      currentMetrics = &LyraMetrics::values;
      break;
  }
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints) {
  const ThemeMetrics& metrics = UITheme::getMetrics();
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight + metrics.verticalSpacing;
  }
  if (hasButtonHints) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight;
  return availableHeight / metrics.listRowHeight;
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

void UITheme::drawButtonHints(const GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                              const char* btn4) {
  if (currentTheme != nullptr) {
    currentTheme->drawButtonHints(renderer, btn1, btn2, btn3, btn4);
  }
}

void UITheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) {
  if (currentTheme != nullptr) {
    currentTheme->drawSideButtonHints(renderer, topBtn, bottomBtn);
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

void UITheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title) {
  if (currentTheme != nullptr) {
    currentTheme->drawHeader(renderer, rect, title);
  }
}

void UITheme::drawTabBar(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs) {
  if (currentTheme != nullptr) {
    currentTheme->drawTabBar(renderer, rect, tabs);
  }
}

void UITheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBookWithCover>& recentBooks,
                                  const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                  bool& bufferRestored, std::function<bool()> storeCoverBuffer) {
  if (currentTheme != nullptr) {
    currentTheme->drawRecentBookCover(renderer, rect, recentBooks, selectorIndex, coverRendered, coverBufferStored,
                                      bufferRestored, storeCoverBuffer);
  }
}

void UITheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                             const std::function<std::string(int index)>& buttonLabel, bool hasIcon,
                             const std::function<std::string(int index)>& rowIcon) {
  if (currentTheme != nullptr) {
    currentTheme->drawButtonMenu(renderer, rect, buttonCount, selectedIndex, buttonLabel, hasIcon, rowIcon);
  }
}

void UITheme::drawPopup(GfxRenderer& renderer, const char* message) {
  if (currentTheme != nullptr) {
    currentTheme->drawPopup(renderer, message);
  }
}

PopupCallbacks UITheme::drawPopupWithProgress(GfxRenderer& renderer, const std::string& title) {
  if (currentTheme != nullptr) {
    return currentTheme->drawPopupWithProgress(renderer, title);
  }
  return PopupCallbacks{};
}

void UITheme::drawBookProgressBar(const GfxRenderer& renderer, const size_t bookProgress) {
  if (currentTheme != nullptr) {
    currentTheme->drawBookProgressBar(renderer, bookProgress);
  }
}