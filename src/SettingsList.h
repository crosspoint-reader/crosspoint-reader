#pragma once

#include <I18n.h>

#include <vector>

#include "CrossPointSettings.h"
#include "activities/settings/SettingsActivity.h"
#include "core/features/FeatureModules.h"

inline bool supportsBackgroundServerModeSetting() {
  return core::FeatureModules::hasCapability(core::Capability::BackgroundServerOnCharge);
}

inline bool supportsBackgroundServerAlwaysSetting() {
  return core::FeatureModules::hasCapability(core::Capability::BackgroundServerAlways);
}

inline std::vector<std::string> backgroundServerModeOptions() {
  std::vector<std::string> options = {std::string(I18N.get(StrId::STR_NEVER)),
                                      std::string(I18N.get(StrId::STR_ONLY_ON_CHARGE))};
  if (supportsBackgroundServerAlwaysSetting()) {
    options.emplace_back(I18N.get(StrId::STR_ALWAYS));
  }
  return options;
}

inline uint8_t getBackgroundServerModeSettingIndex() {
  const uint8_t mode = SETTINGS.getBackgroundServerMode();
  if (supportsBackgroundServerAlwaysSetting()) {
    if (mode == CrossPointSettings::BACKGROUND_SERVER_ALWAYS) {
      return 2;
    }
    if (mode == CrossPointSettings::BACKGROUND_SERVER_ON_CHARGE) {
      return 1;
    }
    return 0;
  }
  return mode == CrossPointSettings::BACKGROUND_SERVER_ON_CHARGE ? 1 : 0;
}

inline void setBackgroundServerModeSettingIndex(const uint8_t index) {
  if (supportsBackgroundServerAlwaysSetting()) {
    SETTINGS.setBackgroundServerMode(index <= CrossPointSettings::BACKGROUND_SERVER_ALWAYS
                                         ? index
                                         : CrossPointSettings::BACKGROUND_SERVER_NEVER);
    return;
  }

  SETTINGS.setBackgroundServerMode(index == 1 ? CrossPointSettings::BACKGROUND_SERVER_ON_CHARGE
                                              : CrossPointSettings::BACKGROUND_SERVER_NEVER);
}

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
inline std::vector<SettingInfo> getSettingsList() {
  std::vector<SettingInfo> list = {
      // --- Display ---
      SettingInfo::Enum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
                        {StrId::STR_DARK, StrId::STR_LIGHT, StrId::STR_FOLLOW_THEME, StrId::STR_CUSTOM, StrId::STR_TRANSPARENT}, "sleepScreen",
                        StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(StrId::STR_SLEEP_SOURCE, &CrossPointSettings::sleepScreenSource,
                        {StrId::STR_SLEEP, StrId::STR_POKEDEX, StrId::STR_ALL}, "sleepScreenSource",
                        StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                        {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                        {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                        "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY),
      SettingInfo::Action(StrId::STR_VALIDATE_SLEEP_IMAGES, SettingAction::ValidateSleepImages),
      SettingInfo::Toggle(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount,
                          "statusBarChapterPageCount", StrId::STR_CUSTOMISE_STATUS_BAR),
      SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarBookProgressPercentage,
                          "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarProgressBar,
                        {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarProgressBar",
                        StrId::STR_CUSTOMISE_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                        {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                        "statusBarProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                        {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarTitle",
                        StrId::STR_CUSTOMISE_STATUS_BAR),
      SettingInfo::Toggle(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery, "statusBarBattery",
                          StrId::STR_CUSTOMISE_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                        {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                        StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(
          StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
          {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15, StrId::STR_PAGES_30},
          "refreshFrequency", StrId::STR_CAT_DISPLAY),
      // Build options with explicit enum-value mapping so position != value assumptions
      // don't break when individual themes are optionally included or excluded.
      [] {
        std::vector<StrId> ids = {StrId::STR_THEME_CLASSIC};
        std::vector<uint8_t> vals = {CrossPointSettings::UI_THEME::CLASSIC};
        if (core::FeatureModules::hasCapability(core::Capability::LyraTheme)) {
          ids.insert(ids.end(),
                     {StrId::STR_THEME_LYRA, StrId::STR_THEME_LYRA_EXTENDED, StrId::STR_THEME_FORK_DRIFT});
          vals.insert(vals.end(), {CrossPointSettings::UI_THEME::LYRA, CrossPointSettings::UI_THEME::LYRA_EXTENDED,
                                   CrossPointSettings::UI_THEME::FORK_DRIFT});
        }
        if (core::FeatureModules::hasCapability(core::Capability::PokemonParty)) {
          ids.push_back(StrId::STR_THEME_POKEMON_PARTY);
          vals.push_back(CrossPointSettings::UI_THEME::POKEMON_PARTY);
        }
        return SettingInfo::DynamicEnum(
            StrId::STR_UI_THEME, std::move(ids),
            [vals] {
              const uint8_t cur = SETTINGS.uiTheme;
              for (size_t i = 0; i < vals.size(); i++) {
                if (vals[i] == cur) return static_cast<uint8_t>(i);
              }
              return uint8_t{0};
            },
            [vals](uint8_t idx) {
              if (idx < vals.size()) SETTINGS.uiTheme = vals[idx];
            },
            "uiTheme", StrId::STR_CAT_DISPLAY);
      }(),
      SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                          StrId::STR_CAT_DISPLAY),

      // --- Reader ---
      SettingInfo::DynamicEnum(
          StrId::STR_FONT_FAMILY,
          [] {
            std::vector<StrId> values = {StrId::STR_BOOKERLY, StrId::STR_NOTO_SANS, StrId::STR_OPEN_DYSLEXIC};
            if (core::FeatureModules::hasCapability(core::Capability::UserFonts)) {
              values.push_back(StrId::STR_EXTERNAL_FONT);
            }
            return values;
          }(),
          [] { return SETTINGS.fontFamily; }, [](uint8_t value) { SETTINGS.fontFamily = value; }, "fontFamily",
          StrId::STR_CAT_READER),
      SettingInfo::Enum(StrId::STR_FONT_SIZE, &CrossPointSettings::fontSize,
                        {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE}, "fontSize",
                        StrId::STR_CAT_READER),
      SettingInfo::Enum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing,
                        {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE}, "lineSpacing", StrId::STR_CAT_READER),
      SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin, {5, 40, 5}, "screenMargin",
                         StrId::STR_CAT_READER),
      SettingInfo::Enum(StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                        {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                         StrId::STR_BOOK_S_STYLE},
                        "paragraphAlignment", StrId::STR_CAT_READER),
      SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                          StrId::STR_CAT_READER),
      SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled",
                          StrId::STR_CAT_READER),
      SettingInfo::Enum(StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
                        {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW},
                        "orientation", StrId::STR_CAT_READER),
      SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing, "extraParagraphSpacing",
                          StrId::STR_CAT_READER),
      SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                          StrId::STR_CAT_READER),
      SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                        {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                        "imageRendering", StrId::STR_CAT_READER),
      // --- Controls ---
      SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                        {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV}, "sideButtonLayout", StrId::STR_CAT_CONTROLS),
      SettingInfo::Toggle(StrId::STR_LONG_PRESS_SKIP, &CrossPointSettings::longPressChapterSkip, "longPressChapterSkip",
                          StrId::STR_CAT_CONTROLS),
      SettingInfo::Enum(StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
                        {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_PAGE_TURN, StrId::STR_SELECT}, "shortPwrBtn",
                        StrId::STR_CAT_CONTROLS),

      // --- System ---
      SettingInfo::Enum(StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeout,
                        {StrId::STR_MIN_1, StrId::STR_MIN_5, StrId::STR_MIN_10, StrId::STR_MIN_15, StrId::STR_MIN_30},
                        "sleepTimeout", StrId::STR_CAT_SYSTEM),
  };

  if (core::FeatureModules::hasCapability(core::Capability::TrmnlSwitch)) {
    list.push_back(SettingInfo::Action(StrId::STR_SWITCH_TO_TRMNL, SettingAction::SwitchToTrmnl));
  }

  if (core::FeatureModules::hasCapability(core::Capability::DarkMode)) {
    list.push_back(
        SettingInfo::Toggle(StrId::STR_DARK_MODE, &CrossPointSettings::darkMode, "darkMode", StrId::STR_CAT_DISPLAY));
  }

  if (core::FeatureModules::hasCapability(core::Capability::GlobalStatusBar)) {
    list.push_back(SettingInfo::Toggle(StrId::STR_GLOBAL_STATUS_BAR, &CrossPointSettings::globalStatusBar,
                                       "globalStatusBar", StrId::STR_CAT_DISPLAY));
    list.push_back(SettingInfo::Enum(StrId::STR_STATUS_BAR_POSITION, &CrossPointSettings::globalStatusBarPosition,
                                     {StrId::STR_STATUS_BAR_TOP, StrId::STR_STATUS_BAR_BOTTOM},
                                     "globalStatusBarPosition", StrId::STR_CAT_DISPLAY));
  }

  if (core::FeatureModules::hasCapability(core::Capability::UsbMassStorage)) {
    list.push_back(SettingInfo::Toggle(StrId::STR_FILE_TRANSFER, &CrossPointSettings::usbMscPromptOnConnect,
                                       "usbMscPromptOnConnect", StrId::STR_CAT_SYSTEM));
  }

  if (supportsBackgroundServerModeSetting()) {
    list.push_back(SettingInfo::DynamicEnum(
        StrId::STR_BACKGROUND_SERVER, {}, [] { return getBackgroundServerModeSettingIndex(); },
        [](uint8_t value) { setBackgroundServerModeSettingIndex(value); }, "backgroundServerMode", StrId::STR_CAT_SYSTEM,
        [] { return backgroundServerModeOptions(); }));
  }

  // Device name for mDNS/DHCP/AP SSID. Editable on-device via keyboard (STRING handler).
  // Input is sanitized to [a-z0-9-], max 24 chars, via validateAndClamp() on save.
  list.push_back(SettingInfo::String(StrId::STR_DEVICE_NAME, SETTINGS.deviceName, sizeof(SETTINGS.deviceName),
                                     "deviceName", StrId::STR_CAT_SYSTEM));

  if (core::FeatureModules::hasCapability(core::Capability::KoreaderSync)) {
    // --- KOReader Sync (web-only, persisted via FeatureModules) ---
    list.push_back(SettingInfo::DynamicString(
        StrId::STR_KOREADER_USERNAME, [] { return core::FeatureModules::getKoreaderUsername(); },
        [](const std::string& value) { core::FeatureModules::setKoreaderUsername(value, false); }, "koUsername",
        StrId::STR_KOREADER_SYNC));
    list.push_back(SettingInfo::DynamicString(
        StrId::STR_KOREADER_PASSWORD, [] { return core::FeatureModules::getKoreaderPassword(); },
        [](const std::string& value) { core::FeatureModules::setKoreaderPassword(value, false); }, "koPassword",
        StrId::STR_KOREADER_SYNC));
    list.push_back(SettingInfo::DynamicString(
        StrId::STR_SYNC_SERVER_URL, [] { return core::FeatureModules::getKoreaderServerUrl(); },
        [](const std::string& value) { core::FeatureModules::setKoreaderServerUrl(value, false); }, "koServerUrl",
        StrId::STR_KOREADER_SYNC));
    list.push_back(SettingInfo::DynamicEnum(
        StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
        [] { return core::FeatureModules::getKoreaderMatchMethod(); },
        [](uint8_t value) { core::FeatureModules::setKoreaderMatchMethod(value, false); }, "koMatchMethod",
        StrId::STR_KOREADER_SYNC));
  }

  if (core::FeatureModules::hasCapability(core::Capability::UserFonts)) {
    list.push_back(SettingInfo::DynamicEnum(
        StrId::STR_EXTERNAL_FONT, {}, [] { return core::FeatureModules::getSelectedUserFontFamilyIndex(); },
        [](uint8_t value) { core::FeatureModules::setSelectedUserFontFamilyIndex(value); }, "userFontPath",
        StrId::STR_CAT_READER, [] { return core::FeatureModules::getUserFontFamilies(); }));
  }

  if (core::FeatureModules::hasCapability(core::Capability::CalibreSync)) {
    // OPDS intentionally binds directly to SETTINGS char arrays because SettingInfo::String
    // edits in-place mutable storage; unlike KOReader credentials, OPDS persistence remains
    // owned by CrossPointSettings/JsonSettingsIO.
    list.push_back(SettingInfo::String(StrId::STR_OPDS_SERVER_URL, SETTINGS.opdsServerUrl,
                                       sizeof(SETTINGS.opdsServerUrl), "opdsServerUrl", StrId::STR_OPDS_BROWSER));
    list.push_back(SettingInfo::String(StrId::STR_USERNAME, SETTINGS.opdsUsername, sizeof(SETTINGS.opdsUsername),
                                       "opdsUsername", StrId::STR_OPDS_BROWSER));
    list.push_back(SettingInfo::String(StrId::STR_PASSWORD, SETTINGS.opdsPassword, sizeof(SETTINGS.opdsPassword),
                                       "opdsPassword", StrId::STR_OPDS_BROWSER)
                       .withObfuscated());
  }

  return list;
}
