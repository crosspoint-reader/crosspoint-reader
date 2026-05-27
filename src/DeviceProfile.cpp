#include "DeviceProfile.h"

#include <HalGPIO.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace {
constexpr ThemeMetrics MurphyLyraMetrics = {.batteryWidth = 16,
                                            .batteryHeight = 12,
                                            .topPadding = 3,
                                            .batteryBarHeight = 22,
                                            .headerHeight = 40,
                                            .verticalSpacing = 7,
                                            .contentSidePadding = 16,
                                            .listRowHeight = 26,
                                            .listWithSubtitleRowHeight = 36,
                                            .menuRowHeight = 26,
                                            .menuSpacing = 8,
                                            .tabSpacing = 6,
                                            .tabBarHeight = 30,
                                            .scrollBarWidth = 3,
                                            .scrollBarRightOffset = 3,
                                            .homeTopPadding = 34,
                                            .homeCoverHeight = 140,
                                            .homeCoverTileHeight = 152,
                                            .homeRecentBooksCount = 1,
                                            .homeContinueReadingInMenu = false,
                                            .homeMenuTopOffset = 12,
                                            .buttonHintsHeight = 0,
                                            .sideButtonHintsWidth = 0,
                                            .progressBarHeight = 10,
                                            .progressBarMarginTop = 1,
                                            .statusBarHorizontalMargin = 4,
                                            .statusBarVerticalMargin = 12,
                                            .keyboardKeyWidth = 22,
                                            .keyboardKeyHeight = 30,
                                            .keyboardKeySpacing = 0,
                                            .keyboardBottomKeyHeight = 28,
                                            .keyboardBottomKeySpacing = 4,
                                            .keyboardBottomAligned = true,
                                            .keyboardCenteredText = false,
                                            .keyboardVerticalOffset = -6,
                                            .keyboardTextFieldWidthPercent = 88,
                                            .keyboardWidthPercent = 94,
                                            .keyboardKeyCornerRadius = 4,
                                            .keyboardFillUnselected = false,
                                            .keyboardOutlineAllUnselected = false,
                                            .keyboardDrawSpecialOutlineWhenUnselected = true,
                                            .keyboardSecondaryLabelRightPadding = 1,
                                            .keyboardSecondaryLabelTopPadding = 0,
                                            .keyboardMinArrowHeadSize = 0,
                                            .popupTopOffsetRatio = 0.12f,
                                            .popupMarginX = 10,
                                            .popupMarginY = 8,
                                            .popupFrameThickness = 1,
                                            .popupCornerRadius = 4,
                                            .popupTextBold = false,
                                            .popupTextInverted = false,
                                            .popupTextBaselineOffsetY = -2,
                                            .popupProgressBarHeight = 3,
                                            .popupProgressDrawOutline = false,
                                            .popupProgressClampPercent = false,
                                            .popupProgressFillInverted = false,
                                            .popupProgressOutlineInverted = false,
                                            .textFieldHorizontalPadding = 5,
                                            .textFieldNormalThickness = 1,
                                            .textFieldCursorThickness = 2,
                                            .textFieldLineEndOffset = 0};

constexpr DeviceProfile X4Profile = {.name = "X4",
                                     .touchPrimary = false,
                                     .showButtonHints = true,
                                     .denseLightGrayDither = false,
                                     .supportsGrayscaleAntiAlias = true,
                                     .lyraMetrics = nullptr,
                                     .coverThumbScale = 1,
                                     .emptyCoverIconSize = 32,
                                     .recentTitleFontId = UI_12_FONT_ID,
                                     .recentAuthorFontId = UI_10_FONT_ID,
                                     .emptyRecentsTitleFontId = UI_12_FONT_ID,
                                     .emptyRecentsSubtitleFontId = UI_10_FONT_ID,
                                     .mainMenuIconDrawSize = 32,
                                     .mainMenuTextInset = 16,
                                     .mainMenuLabelFontId = UI_12_FONT_ID,
                                     .rotateMainMenuIconsClockwise = false,
                                     .readerFontSizeDelta = 0};

constexpr DeviceProfile X3Profile = {.name = "X3",
                                     .touchPrimary = false,
                                     .showButtonHints = true,
                                     .denseLightGrayDither = false,
                                     .supportsGrayscaleAntiAlias = true,
                                     .lyraMetrics = nullptr,
                                     .coverThumbScale = 1,
                                     .emptyCoverIconSize = 32,
                                     .recentTitleFontId = UI_12_FONT_ID,
                                     .recentAuthorFontId = UI_10_FONT_ID,
                                     .emptyRecentsTitleFontId = UI_12_FONT_ID,
                                     .emptyRecentsSubtitleFontId = UI_10_FONT_ID,
                                     .mainMenuIconDrawSize = 32,
                                     .mainMenuTextInset = 16,
                                     .mainMenuLabelFontId = UI_12_FONT_ID,
                                     .rotateMainMenuIconsClockwise = false,
                                     .readerFontSizeDelta = 0};

constexpr DeviceProfile TouchMurphyProfile = {.name = "Murphy M3",
                                              .touchPrimary = true,
                                              .showButtonHints = false,
                                              .denseLightGrayDither = true,
                                              .supportsGrayscaleAntiAlias = false,
                                              .lyraMetrics = &MurphyLyraMetrics,
                                              .coverThumbScale = 2,
                                              .emptyCoverIconSize = 22,
                                              .recentTitleFontId = SMALL_FONT_ID,
                                              .recentAuthorFontId = SMALL_FONT_ID,
                                              .emptyRecentsTitleFontId = SMALL_FONT_ID,
                                              .emptyRecentsSubtitleFontId = SMALL_FONT_ID,
                                              .mainMenuIconDrawSize = 20,
                                              .mainMenuTextInset = 10,
                                              .mainMenuLabelFontId = SMALL_FONT_ID,
                                              .rotateMainMenuIconsClockwise = true,
                                              .readerFontSizeDelta = -1};

int builtinReaderFontId(uint8_t fontFamily, uint8_t fontSize) {
  switch (fontFamily) {
    case CrossPointSettings::NOTOSERIF:
    default:
      switch (fontSize) {
        case CrossPointSettings::SMALL:
          return NOTOSERIF_12_FONT_ID;
        case CrossPointSettings::MEDIUM:
        default:
          return NOTOSERIF_14_FONT_ID;
        case CrossPointSettings::LARGE:
          return NOTOSERIF_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE:
          return NOTOSERIF_18_FONT_ID;
      }
    case CrossPointSettings::NOTOSANS:
      switch (fontSize) {
        case CrossPointSettings::SMALL:
          return NOTOSANS_12_FONT_ID;
        case CrossPointSettings::MEDIUM:
        default:
          return NOTOSANS_14_FONT_ID;
        case CrossPointSettings::LARGE:
          return NOTOSANS_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE:
          return NOTOSANS_18_FONT_ID;
      }
    case CrossPointSettings::OPENDYSLEXIC:
      switch (fontSize) {
        case CrossPointSettings::SMALL:
          return OPENDYSLEXIC_8_FONT_ID;
        case CrossPointSettings::MEDIUM:
        default:
          return OPENDYSLEXIC_10_FONT_ID;
        case CrossPointSettings::LARGE:
          return OPENDYSLEXIC_12_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE:
          return OPENDYSLEXIC_14_FONT_ID;
      }
  }
}
}  // namespace

const DeviceProfile& DeviceProfiles::current() {
  if (gpio.deviceIsMurphyM3()) {
    return TouchMurphyProfile;
  }
  if (gpio.deviceIsX3()) {
    return X3Profile;
  }
  return X4Profile;
}

uint8_t DeviceProfiles::effectiveReaderFontSize() {
  const int rawSize = std::clamp<int>(SETTINGS.fontSize, 0, CrossPointSettings::FONT_SIZE_COUNT - 1);
  const int adjusted = std::clamp(rawSize + current().readerFontSizeDelta, 0, CrossPointSettings::FONT_SIZE_COUNT - 1);
  return static_cast<uint8_t>(adjusted);
}

int DeviceProfiles::readerFontId() {
  const uint8_t effectiveSize = effectiveReaderFontSize();
  if (SETTINGS.sdFontFamilyName[0] != '\0' && SETTINGS.sdFontIdResolver) {
    const int id = SETTINGS.sdFontIdResolver(SETTINGS.sdFontResolverCtx, SETTINGS.sdFontFamilyName, effectiveSize);
    if (id != 0) {
      return id;
    }
  }
  return builtinReaderFontId(SETTINGS.fontFamily, effectiveSize);
}

int DeviceProfiles::readerScreenMargin() { return SETTINGS.screenMargin; }
