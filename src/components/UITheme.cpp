#include "UITheme.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/lyra/LyraTheme.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
}  // namespace

UITheme UITheme::instance;

UITheme::UITheme() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::reload() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  switch (type) {
    case CrossPointSettings::UI_THEME::CLASSIC:
      LOG_DBG("UI", "Using Classic theme");
      currentTheme = new BaseTheme();
      currentMetrics = &BaseMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA:
      LOG_DBG("UI", "Using Lyra theme");
      currentTheme = new LyraTheme();
      currentMetrics = &LyraMetrics::values;
      break;
  }
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight;
  }
  if (hasButtonHints) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight;
  int rowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  return availableHeight / rowHeight;
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

void UITheme::handleListScrolling(const GfxRenderer& renderer, int listSize, int pageItems, size_t& selectorIndex,
                                  const MappedInputManager& mappedInput, bool& updateRequired) {
  const bool upReleased = mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                          mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool downReleased = mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Down);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;

  if (upReleased) {
    if (skipPage) {
      selectorIndex = std::max(static_cast<int>((selectorIndex / pageItems - 1) * pageItems), 0);
    } else {
      selectorIndex = (selectorIndex + listSize - 1) % listSize;
    }
    updateRequired = true;
  } else if (downReleased) {
    if (skipPage) {
      selectorIndex = std::min(static_cast<int>((selectorIndex / pageItems + 1) * pageItems), listSize - 1);
    } else {
      selectorIndex = (selectorIndex + 1) % listSize;
    }
    updateRequired = true;
  }
}
