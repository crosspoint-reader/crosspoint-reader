#include "UITheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
constexpr int STATUS_BAR_ITEM_PADDING = 4;
constexpr int STATUS_BAR_DESCENDER_CLEARANCE = 4;
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
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &BaseMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA:
      LOG_DBG("UI", "Using Lyra theme");
      currentTheme = std::make_unique<LyraTheme>();
      currentMetrics = &LyraMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::ROUNDEDRAFF:
      LOG_DBG("UI", "Using RoundedRaff theme");
      currentTheme = std::make_unique<RoundedRaffTheme>();
      currentMetrics = &RoundedRaffMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
      LOG_DBG("UI", "Using Lyra 3 Covers theme");
      currentTheme = std::make_unique<Lyra3CoversTheme>();
      currentMetrics = &Lyra3CoversMetrics::values;
      break;
  }
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight) {
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
  const int availableHeight = renderer.getScreenHeight() - reservedHeight - extraReservedHeight;
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

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename)) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasBmpExtension(filename)) {
    return Image;
  }
  return File;
}

int UITheme::getProgressBarHeight(const uint8_t progressBar, const uint8_t thickness) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  if (progressBar == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS) {
    return 0;
  }

  return ((thickness + 1) * 2) + metrics.progressBarMarginTop;
}

int UITheme::getStatusBarItemsHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  return std::max(metrics.statusBarVerticalMargin + (STATUS_BAR_ITEM_PADDING * 2) + STATUS_BAR_DESCENDER_CLEARANCE,
                  metrics.batteryHeight + (STATUS_BAR_ITEM_PADDING * 2) + STATUS_BAR_DESCENDER_CLEARANCE);
}

int UITheme::getStatusBarTopHeight(const bool forceStatusItems) {
  const bool showStatusItems =
      forceStatusItems || SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage ||
      SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE || SETTINGS.statusBarBattery;
  const bool statusItemsAtTop =
      SETTINGS.statusBarItemsPosition == CrossPointSettings::STATUS_BAR_ITEMS_POSITION::STATUS_BAR_ITEMS_TOP;
  const int statusItemsHeight = showStatusItems && statusItemsAtTop ? getStatusBarItemsHeight() : 0;

  return getProgressBarHeight(SETTINGS.statusBarUpperProgressBar, SETTINGS.statusBarUpperProgressBarThickness) +
         statusItemsHeight;
}

int UITheme::getStatusBarBottomHeight(const bool forceStatusItems) {
  const bool showStatusItems =
      forceStatusItems || SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage ||
      SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE || SETTINGS.statusBarBattery;
  const bool statusItemsAtBottom =
      SETTINGS.statusBarItemsPosition == CrossPointSettings::STATUS_BAR_ITEMS_POSITION::STATUS_BAR_ITEMS_BOTTOM;
  const int statusItemsHeight = showStatusItems && statusItemsAtBottom ? getStatusBarItemsHeight() : 0;

  return getProgressBarHeight(SETTINGS.statusBarLowerProgressBar, SETTINGS.statusBarLowerProgressBarThickness) +
         statusItemsHeight;
}

int UITheme::getStatusBarHeight(const bool forceStatusItems) {
  return getStatusBarTopHeight(forceStatusItems) + getStatusBarBottomHeight(forceStatusItems);
}
