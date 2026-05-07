#include "UITheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "components/themes/lyra/LyraTheme.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
constexpr int STATUS_BAR_ITEM_PADDING = 4;
constexpr int STATUS_BAR_DESCENDER_CLEARANCE = 4;

uint8_t normalizeProgressBar(const uint8_t progressBar) {
  return progressBar < CrossPointSettings::STATUS_BAR_PROGRESS_BAR_COUNT ? progressBar
                                                                         : CrossPointSettings::HIDE_PROGRESS;
}

uint8_t normalizeProgressBarThickness(const uint8_t thickness) {
  return thickness < CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT
             ? thickness
             : CrossPointSettings::PROGRESS_BAR_NORMAL;
}

uint8_t normalizeStatusBarItemsPosition(const uint8_t position) {
  return position < CrossPointSettings::STATUS_BAR_ITEMS_POSITION_COUNT ? position
                                                                        : CrossPointSettings::STATUS_BAR_ITEMS_BOTTOM;
}
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
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
      LOG_DBG("UI", "Using Lyra 3 Covers theme");
      currentTheme = std::make_unique<Lyra3CoversTheme>();
      currentMetrics = &Lyra3CoversMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_CAROUSEL:
      LOG_DBG("UI", "Using Lyra Carousel theme");
      currentTheme = std::make_unique<LyraCarouselTheme>();
      currentMetrics = &LyraCarouselMetrics::values;
      break;
    default:
      LOG_ERR("UI", "Unknown theme %d, falling back to Classic", static_cast<int>(type));
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &BaseMetrics::values;
      break;
  }
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = getContentRect(renderer, hasButtonHints, /*hasSideHints=*/false);
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight;
  }
  if (hasButtonHints) {
    reservedHeight += metrics.verticalSpacing;
  }
  const int availableHeight = contentRect.height - reservedHeight;
  int rowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  return availableHeight / rowHeight;
}

Rect UITheme::getContentRect(const GfxRenderer& renderer, bool hasBottomHints, bool hasSideHints) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const int bh = hasBottomHints ? metrics.buttonHintsHeight : 0;
  const int sw = hasSideHints ? metrics.sideButtonHintsWidth : 0;

  int top = 0, right = 0, bottom = 0, left = 0;
  switch (renderer.getOrientation()) {
    case GfxRenderer::Portrait:
      bottom = bh;
      if (gpio.deviceIsX3() && hasSideHints) {
        left = sw;
        right = sw;
      } else {
        right = sw;
      }
      break;
    case GfxRenderer::PortraitInverted:
      top = bh;
      if (gpio.deviceIsX3() && hasSideHints) {
        left = sw;
        right = sw;
      } else {
        left = sw;
      }
      break;
    case GfxRenderer::LandscapeClockwise:
      left = bh;
      if (gpio.deviceIsX3() && hasSideHints) {
        top = sw;
        bottom = sw;
      } else {
        bottom = sw;
      }
      break;
    case GfxRenderer::LandscapeCounterClockwise:
      right = bh;
      if (gpio.deviceIsX3() && hasSideHints) {
        top = sw;
        bottom = sw;
      } else {
        top = sw;
      }
      break;
  }

  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  return Rect{left, top, w - left - right, h - top - bottom};
}

std::string UITheme::makeSeparatorTitle(const std::string& title) { return std::string("__") + title; }

std::string UITheme::makeSeparatorTitle(StrId labelId) { return std::string("__") + I18N.get(labelId); }

bool UITheme::isSeparatorTitle(const std::string& title) { return title.rfind("__", 0) == 0; }

std::string UITheme::stripSeparatorTitle(const std::string& title) {
  return isSeparatorTitle(title) ? title.substr(2) : title;
}

std::function<bool(int)> UITheme::makeSelectablePredicate(int total, std::function<std::string(int)> titleGetter) {
  return
      [total, titleGetter](int index) { return index >= 0 && index < total && !isSeparatorTitle(titleGetter(index)); };
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int width, int height) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(width) + "x" + std::to_string(height));
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
  if (FsHelpers::hasBmpExtension(filename) || FsHelpers::hasJpgExtension(filename) ||
      FsHelpers::hasPngExtension(filename)) {
    return Image;
  }
  return File;
}

int UITheme::getProgressBarHeight(const uint8_t progressBar, const uint8_t thickness) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const uint8_t normalizedProgressBar = normalizeProgressBar(progressBar);
  if (normalizedProgressBar == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS) {
    return 0;
  }
  const uint8_t normalizedThickness = normalizeProgressBarThickness(thickness);
  return ((normalizedThickness + 1) * 2) + metrics.progressBarMarginTop;
}

int UITheme::getStatusBarItemsHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  return std::max(metrics.statusBarVerticalMargin + (STATUS_BAR_ITEM_PADDING * 2) + STATUS_BAR_DESCENDER_CLEARANCE,
                  metrics.batteryHeight + (STATUS_BAR_ITEM_PADDING * 2) + STATUS_BAR_DESCENDER_CLEARANCE);
}

int UITheme::getStatusBarTopHeight(const bool forceStatusItems) {
  const bool showStatusItems = forceStatusItems || SETTINGS.statusBarChapterPageCount ||
                               SETTINGS.statusBarBookProgressPercentage ||
                               SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE ||
                               SETTINGS.statusBarBattery || (SETTINGS.useClock && SETTINGS.statusBarClock);
  const uint8_t statusBarItemsPosition = normalizeStatusBarItemsPosition(SETTINGS.statusBarItemsPosition);
  const bool statusItemsAtTop =
      statusBarItemsPosition == CrossPointSettings::STATUS_BAR_ITEMS_POSITION::STATUS_BAR_ITEMS_TOP;
  const int statusItemsHeight = showStatusItems && statusItemsAtTop ? getStatusBarItemsHeight() : 0;
  return getProgressBarHeight(SETTINGS.statusBarUpperProgressBar, SETTINGS.statusBarUpperProgressBarThickness) +
         statusItemsHeight;
}

int UITheme::getStatusBarBottomHeight(const bool forceStatusItems) {
  const bool showStatusItems = forceStatusItems || SETTINGS.statusBarChapterPageCount ||
                               SETTINGS.statusBarBookProgressPercentage ||
                               SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE ||
                               SETTINGS.statusBarBattery || (SETTINGS.useClock && SETTINGS.statusBarClock);
  const uint8_t statusBarItemsPosition = normalizeStatusBarItemsPosition(SETTINGS.statusBarItemsPosition);
  const bool statusItemsAtBottom =
      statusBarItemsPosition == CrossPointSettings::STATUS_BAR_ITEMS_POSITION::STATUS_BAR_ITEMS_BOTTOM;
  const int statusItemsHeight = showStatusItems && statusItemsAtBottom ? getStatusBarItemsHeight() : 0;
  return getProgressBarHeight(SETTINGS.statusBarLowerProgressBar, SETTINGS.statusBarLowerProgressBarThickness) +
         statusItemsHeight;
}

int UITheme::getStatusBarHeight(const bool forceStatusItems) {
  return getStatusBarTopHeight(forceStatusItems) + getStatusBarBottomHeight(forceStatusItems);
}
