#include "UITheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "components/themes/BaseTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"

UITheme UITheme::instance;

namespace {

// Round a pixel dimension by a scale factor.
int sp(int v, float s) { return static_cast<int>(std::lround(v * s)); }

}  // namespace

// Density scale is 1.0 here (button devices); touch builds override this to wire
// in the per-board profile. Resolution scaling composes with it (see reload()).
float UITheme::uiScale() { return 1.0f; }

// Unified theme metric scaling. Two independent, composable inputs:
//   res     - resolution ratio: this panel's pixels vs the theme's design
//             resolution. Applies to EVERY pixel field, including the home cover
//             and reader chrome, because more/fewer pixels means the whole layout
//             scales proportionally and still fits by construction.
//   density - per-board UI/touch-target scale (uiScale: 1.0 on button devices,
//             >1 on high-density touch boards). Same pixel count, so it applies
//             ONLY to chrome that can grow into spare space, and is deliberately
//             withheld from fit-constrained elements (home cover, reader status/
//             progress bars) that would overflow the fixed panel if enlarged.
// Effective factor per field is res (always) times density (where eligible).
// Counts, percents, ratios, and bools are never scaled. Degrades exactly: with
// density==1 this is pure resolution scaling; with res==1, pure density scaling.
ThemeMetrics scaleThemeMetrics(const ThemeMetrics& b, float res, float density) {
  static_assert(sizeof(ThemeMetrics) == THEME_METRICS_SIZEOF,
                "ThemeMetrics changed: review scaleThemeMetrics() and update THEME_METRICS_SIZEOF");
  ThemeMetrics m = b;
  const float full = res * density;  // resolution + density-eligible chrome
  if (res == 1.0f && density == 1.0f) return m;
  m.batteryWidth = sp(b.batteryWidth, full);
  m.batteryHeight = sp(b.batteryHeight, full);
  m.topPadding = sp(b.topPadding, full);
  m.batteryBarHeight = sp(b.batteryBarHeight, full);
  m.headerHeight = sp(b.headerHeight, full);
  m.verticalSpacing = sp(b.verticalSpacing, full);
  m.previewPadding = sp(b.previewPadding, full);  // previewHeightPercent is a percent: not scaled
  m.contentSidePadding = sp(b.contentSidePadding, full);
  m.listRowHeight = sp(b.listRowHeight, full);
  m.listWithSubtitleRowHeight = sp(b.listWithSubtitleRowHeight, full);
  m.menuRowHeight = sp(b.menuRowHeight, full);
  m.menuSpacing = sp(b.menuSpacing, full);
  m.tabSpacing = sp(b.tabSpacing, full);
  m.tabBarHeight = sp(b.tabBarHeight, full);
  m.scrollBarWidth = sp(b.scrollBarWidth, full);
  m.scrollBarRightOffset = sp(b.scrollBarRightOffset, full);
  m.homeTopPadding = sp(b.homeTopPadding, full);
  // Fit-constrained: resolution only, never density (would push the menu off the
  // fixed-height home screen). homeRecentBooksCount/bools are not scaled.
  m.homeCoverHeight = sp(b.homeCoverHeight, res);
  m.homeCoverTileHeight = sp(b.homeCoverTileHeight, res);
  m.homeMenuTopOffset = sp(b.homeMenuTopOffset, full);
  m.buttonHintsHeight = sp(b.buttonHintsHeight, full);
  m.sideButtonHintsWidth = sp(b.sideButtonHintsWidth, full);
  // Reader chrome (compact, uses the un-remapped SMALL font): resolution only,
  // never density, so it does not eat reading area on high-density boards.
  m.progressBarHeight = sp(b.progressBarHeight, res);
  m.progressBarMarginTop = sp(b.progressBarMarginTop, res);
  m.statusBarHorizontalMargin = sp(b.statusBarHorizontalMargin, res);
  m.statusBarVerticalMargin = sp(b.statusBarVerticalMargin, res);
  m.keyboardKeyWidth = sp(b.keyboardKeyWidth, full);
  m.keyboardKeyHeight = sp(b.keyboardKeyHeight, full);
  m.keyboardKeySpacing = sp(b.keyboardKeySpacing, full);
  m.keyboardBottomKeyHeight = sp(b.keyboardBottomKeyHeight, full);
  m.keyboardBottomKeySpacing = sp(b.keyboardBottomKeySpacing, full);
  m.keyboardVerticalOffset = sp(b.keyboardVerticalOffset, full);
  // keyboardTextFieldWidthPercent / keyboardWidthPercent are percents: not scaled
  m.keyboardKeyCornerRadius = sp(b.keyboardKeyCornerRadius, full);
  m.keyboardSecondaryLabelRightPadding = sp(b.keyboardSecondaryLabelRightPadding, full);
  m.keyboardSecondaryLabelTopPadding = sp(b.keyboardSecondaryLabelTopPadding, full);
  m.keyboardMinArrowHeadSize = sp(b.keyboardMinArrowHeadSize, full);
  // popupTopOffsetRatio is a ratio: not scaled
  m.popupMarginX = sp(b.popupMarginX, full);
  m.popupMarginY = sp(b.popupMarginY, full);
  m.popupFrameThickness = sp(b.popupFrameThickness, full);
  m.popupCornerRadius = sp(b.popupCornerRadius, full);
  m.popupTextBaselineOffsetY = sp(b.popupTextBaselineOffsetY, full);
  m.popupProgressBarHeight = sp(b.popupProgressBarHeight, full);
  m.textFieldHorizontalPadding = sp(b.textFieldHorizontalPadding, full);
  m.textFieldNormalThickness = sp(b.textFieldNormalThickness, full);
  m.textFieldCursorThickness = sp(b.textFieldCursorThickness, full);
  m.textFieldLineEndOffset = sp(b.textFieldLineEndOffset, full);
  return m;
}

namespace {

// Scale the cover-strip slot geometry. Covers are fit-constrained, so like
// homeCover they take the resolution factor only, never density. widthPercent is
// a ratio of the cover height and stays unscaled; only pixel offsets/heights move.
void scaleHomeRecents(ThemeHomeRecentsSpec& spec, float res) {
  if (res == 1.0f) return;
  spec.panelCornerRadius = sp(spec.panelCornerRadius, res);
  spec.panelInsetX = sp(spec.panelInsetX, res);
  spec.selectionCornerRadius = sp(spec.selectionCornerRadius, res);
  for (auto& slot : spec.slots) {
    slot.height = sp(slot.height, res);
    slot.xOffset = sp(slot.xOffset, res);
    slot.yOffset = sp(slot.yOffset, res);
    slot.title.offsetY = sp(slot.title.offsetY, res);
  }
}

// Resolution scale = (device native portrait panel) / (theme's declared design
// resolution). Uniform min(width,height) ratio: no axis distortion, content fits
// the tighter axis. Returns 1.0 when constraints are missing (back-compat no-op).
float resolutionScale(const SdThemeDeviceConstraints& design) {
  if (design.screenWidth <= 0 || design.screenHeight <= 0) return 1.0f;
  // Native portrait dimensions are fixed per device (X3 528x792, X4 480x800).
  const int actualW = gpio.deviceIsX3() ? 528 : 480;
  const int actualH = gpio.deviceIsX3() ? 792 : 800;
  const float wRatio = static_cast<float>(actualW) / static_cast<float>(design.screenWidth);
  const float hRatio = static_cast<float>(actualH) / static_cast<float>(design.screenHeight);
  return std::min(wRatio, hRatio);
}

}  // namespace

UITheme::UITheme() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::clearSdThemeState() {
  currentSdMetrics = ThemeMetrics{};
  currentSdHomeRecents = ThemeHomeRecentsSpec{};
  currentSdButtonMenu = ThemeButtonMenuSpec{};
  currentSdList = ThemeListSpec{};
  currentSdButtonHints = ThemeButtonHintsSpec{};
  currentSdTabBar = ThemeTabBarSpec{};
  currentSdHeader = ThemeHeaderSpec{};
  currentSdHomeScreen = ThemeHomeScreenSpec{};
  currentSdFileBrowserScreen = ThemeScreenSpec{};
  currentSdRecentBooksScreen = ThemeScreenSpec{};
  currentSdSettingsScreen = ThemeScreenSpec{};
  currentSdReaderScreen = ThemeScreenSpec{};
  currentSdReaderChrome = ThemeReaderChromeSpec{};
  currentSdThemePath.clear();
  currentSdIcons.clear();
}

void UITheme::refreshRegistry() { themeRegistry.discover(); }

void UITheme::releaseSdThemeAssetMemory() {
  // Keep active SD theme backing storage intact; currentTheme may hold pointers
  // into it. This only releases discovered theme metadata that can be rebuilt.
  themeRegistry.clear();
}

std::vector<int> UITheme::getHomeCoverThumbHeights() const {
  std::vector<int> heights;
  heights.reserve(1 + currentSdHomeRecents.slots.size());
  auto addHeight = [&heights](int height) {
    if (height > 0 && std::find(heights.begin(), heights.end(), height) == heights.end()) {
      heights.push_back(height);
    }
  };

  addHeight(currentMetrics->homeCoverHeight);
  if (currentSdHomeRecents.type == ThemeHomeRecentsType::CoverStrip) {
    for (const auto& slot : currentSdHomeRecents.slots) {
      addHeight(slot.height);
    }
  }
  if (currentSdHomeScreen.enabled) {
    for (const auto& widget : currentSdHomeScreen.widgets) {
      if (widget.type == ThemeHomeWidgetType::FeaturedBookCard) {
        addHeight(widget.featured.coverHeight);
      }
      if (widget.type == ThemeHomeWidgetType::RecentCoverGrid) {
        addHeight(widget.coverGrid.coverHeight);
      }
    }
  }
  if (currentSdRecentBooksScreen.enabled) {
    for (const auto& widget : currentSdRecentBooksScreen.widgets) {
      if (widget.type == ThemeScreenWidgetType::CoverGrid) {
        addHeight(widget.coverGrid.coverHeight);
      }
    }
  }
  if (heights.empty()) return {};
  return {*std::max_element(heights.begin(), heights.end())};
}

int UITheme::getHomeCoverThumbHeight() const {
  const auto heights = getHomeCoverThumbHeights();
  return heights.empty() ? 0 : heights.front();
}

int UITheme::getRecentBooksCoverThumbHeight() const { return getHomeCoverThumbHeight(); }

const ThemeScreenSpec* UITheme::getScreenSpec(ThemeScreenKind screen) const {
  switch (screen) {
    case ThemeScreenKind::FileBrowser:
      return currentSdFileBrowserScreen.enabled ? &currentSdFileBrowserScreen : nullptr;
    case ThemeScreenKind::RecentBooks:
      return currentSdRecentBooksScreen.enabled ? &currentSdRecentBooksScreen : nullptr;
    case ThemeScreenKind::Settings:
      return currentSdSettingsScreen.enabled ? &currentSdSettingsScreen : nullptr;
    case ThemeScreenKind::Reader:
      return currentSdReaderScreen.enabled ? &currentSdReaderScreen : nullptr;
    case ThemeScreenKind::Home:
    default:
      return nullptr;
  }
}

void UITheme::reload() {
  if (SETTINGS.sdThemeName[0] != '\0') {
    const SdCardThemeInfo* themeInfo = themeRegistry.findTheme(SETTINGS.sdThemeName);
    if (themeInfo == nullptr) {
      refreshRegistry();
      themeInfo = themeRegistry.findTheme(SETTINGS.sdThemeName);
    }
    if (themeInfo == nullptr) {
      LOG_ERR("UI", "SD theme not found: %s (falling back to built-in theme)", SETTINGS.sdThemeName);
      themeRegistry.clear();
      SETTINGS.sdThemeName[0] = '\0';
      SETTINGS.saveToFile();
      setTheme(static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme));
      return;
    }

    LOG_DBG("UI", "Using SD theme: %s recentsType=%d count=%d slots=%d", themeInfo->id.c_str(),
            static_cast<int>(themeInfo->homeRecents.type), themeInfo->metrics.homeRecentBooksCount,
            static_cast<int>(themeInfo->homeRecents.slots.size()));
    // Adapt the theme (authored at its declared design resolution) to this panel:
    // resolution ratio for everything, plus the per-board density scale for chrome.
    const float res = resolutionScale(themeInfo->constraints);
    LOG_DBG("UI", "Theme scale: res %d.%03d density %d.%03d (design %dx%d)", static_cast<int>(res),
            static_cast<int>(res * 1000) % 1000, static_cast<int>(uiScale()), static_cast<int>(uiScale() * 1000) % 1000,
            themeInfo->constraints.screenWidth, themeInfo->constraints.screenHeight);
    currentSdMetrics = scaleThemeMetrics(themeInfo->metrics, res, uiScale());
    currentSdHomeRecents = themeInfo->homeRecents;
    scaleHomeRecents(currentSdHomeRecents, res);
    currentSdButtonMenu = themeInfo->buttonMenu;
    currentSdList = themeInfo->list;
    currentSdButtonHints = themeInfo->buttonHints;
    currentSdTabBar = themeInfo->tabBar;
    currentSdHeader = themeInfo->header;
    currentSdHomeScreen = themeInfo->homeScreen;
    currentSdFileBrowserScreen = themeInfo->fileBrowserScreen;
    currentSdRecentBooksScreen = themeInfo->recentBooksScreen;
    currentSdSettingsScreen = themeInfo->settingsScreen;
    currentSdReaderScreen = themeInfo->readerScreen;
    currentSdReaderChrome = themeInfo->readerChrome;
    currentSdThemePath = themeInfo->path;
    currentSdIcons = themeInfo->icons;
    const bool inheritsClassic = themeInfo->inherits == "classic";
    themeRegistry.clear();
    if (inheritsClassic) {
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &currentSdMetrics;
      return;
    }
    const ThemeHomeRecentsSpec* homeRecents =
        currentSdHomeRecents.type != ThemeHomeRecentsType::Default ? &currentSdHomeRecents : nullptr;
    const ThemeButtonMenuSpec* buttonMenu = currentSdButtonMenu.enabled ? &currentSdButtonMenu : nullptr;
    const ThemeListSpec* list = currentSdList.enabled ? &currentSdList : nullptr;
    const ThemeButtonHintsSpec* buttonHints = currentSdButtonHints.enabled ? &currentSdButtonHints : nullptr;
    const ThemeTabBarSpec* tabBar = currentSdTabBar.enabled ? &currentSdTabBar : nullptr;
    const ThemeHeaderSpec* header = currentSdHeader.enabled ? &currentSdHeader : nullptr;
    currentTheme = std::make_unique<LyraTheme>(&currentSdMetrics, homeRecents, buttonMenu, list, buttonHints, tabBar,
                                               header, currentSdThemePath.c_str(), &currentSdIcons);
    currentMetrics = &currentSdMetrics;
    return;
  }

  setTheme(static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme));
}

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  std::unique_ptr<BaseTheme> nextTheme;
  const ThemeMetrics* nextMetrics = &LyraMetrics::values;

  switch (type) {
    case CrossPointSettings::UI_THEME::CLASSIC:
      LOG_DBG("UI", "Using Classic theme");
      nextTheme = std::make_unique<BaseTheme>();
      nextMetrics = &BaseMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA:
      LOG_DBG("UI", "Using Lyra theme");
      nextTheme = std::make_unique<LyraTheme>();
      nextMetrics = &LyraMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::ROUNDEDRAFF:
      LOG_DBG("UI", "Using RoundedRaff theme");
      nextTheme = std::make_unique<RoundedRaffTheme>();
      nextMetrics = &RoundedRaffMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
      LOG_DBG("UI", "Using Lyra 3 Covers theme");
      nextTheme = std::make_unique<Lyra3CoversTheme>();
      nextMetrics = &Lyra3CoversMetrics::values;
      break;
    default:
      LOG_DBG("UI", "Using Lyra theme");
      nextTheme = std::make_unique<LyraTheme>();
      nextMetrics = &LyraMetrics::values;
      break;
  }

  currentTheme = std::move(nextTheme);
  currentMetrics = nextMetrics;
  clearSdThemeState();
  themeRegistry.clear();
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  auto orientation = renderer.getOrientation();
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight;
  }
  if (hasButtonHints && orientation != GfxRenderer::Orientation::LandscapeClockwise &&
      orientation != GfxRenderer::Orientation::LandscapeCounterClockwise) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight - extraReservedHeight;
  int rowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  return availableHeight / rowHeight;
}

// Screen area excluding the button hints
Rect UITheme::getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints, bool hasSideButtonHints) {
  auto orientation = renderer.getOrientation();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  Rect safeArea = Rect{0, 0, screenWidth, screenHeight};
  switch (orientation) {
    case GfxRenderer::Orientation::Portrait:
      if (hasFrontButtonHints) {
        safeArea.height -= currentMetrics->buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeClockwise:
      if (hasFrontButtonHints) {
        safeArea.x += currentMetrics->buttonHintsHeight;
        safeArea.width -= currentMetrics->buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::PortraitInverted:
      if (hasFrontButtonHints) {
        safeArea.y += currentMetrics->buttonHintsHeight;
        safeArea.height -= currentMetrics->buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      if (hasFrontButtonHints) {
        safeArea.width -= currentMetrics->buttonHintsHeight;
      }
      break;
  }
  return safeArea;
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

int UITheme::getStatusBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  // Add status bar margin
  const bool showStatusBar =
      SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage ||
      SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE || SETTINGS.statusBarBattery ||
      SETTINGS.statusBarClock != CrossPointSettings::STATUS_BAR_CLOCK_MODE::STATUS_BAR_CLOCK_HIDE;
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showStatusBar ? (metrics.statusBarVerticalMargin) : 0) +
         (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}

int UITheme::getProgressBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}

// Centered text implementation that takes the safe area into account
void UITheme::drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black, EpdFontFamily::Style style) {
  const int x = screen.x + (screen.width - renderer.getTextWidth(fontId, text, style)) / 2;
  renderer.drawText(fontId, x, y, text, black, style);
}
