#pragma once

#include <I18n.h>

#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "activities/settings/SettingsActivity.h"

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
inline std::vector<SettingInfo> getSettingsList() {
  return {
      // --- Display ---
      SettingInfo::Enum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
                        {StrId::STR_DARK, StrId::STR_LIGHT, StrId::STR_CUSTOM, StrId::STR_COVER, StrId::STR_NONE_OPT,
                         StrId::STR_COVER_CUSTOM},
                        "sleepScreen", StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                        {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                        {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                        "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(
          StrId::STR_STATUS_BAR, &CrossPointSettings::statusBar,
          {StrId::STR_NONE_OPT, StrId::STR_NO_PROGRESS, StrId::STR_STATUS_BAR_FULL_PERCENT,
           StrId::STR_STATUS_BAR_FULL_BOOK, StrId::STR_STATUS_BAR_BOOK_ONLY, StrId::STR_STATUS_BAR_FULL_CHAPTER},
          "statusBar", StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                        {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                        StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(
          StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
          {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15, StrId::STR_PAGES_30},
          "refreshFrequency", StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(StrId::STR_UI_THEME, &CrossPointSettings::uiTheme,
                        {StrId::STR_THEME_CLASSIC, StrId::STR_THEME_LYRA, StrId::STR_THEME_LYRA_EXTENDED}, "uiTheme",
                        StrId::STR_CAT_DISPLAY),
      SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                          StrId::STR_CAT_DISPLAY),

      // --- Reader ---
      SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily,
                        {StrId::STR_BOOKERLY, StrId::STR_NOTO_SANS, StrId::STR_OPEN_DYSLEXIC}, "fontFamily",
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

      // --- Controls ---
      SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                        {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV}, "sideButtonLayout", StrId::STR_CAT_CONTROLS),
      SettingInfo::Enum(StrId::STR_LONG_PRESS_BEHAVIOR, &CrossPointSettings::longPressButtonBehavior,
                        {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                         StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION},
                        "longPressButtonBehavior", StrId::STR_CAT_CONTROLS),
      SettingInfo::Enum(StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
                        {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_PAGE_TURN}, "shortPwrBtn",
                        StrId::STR_CAT_CONTROLS),

      // --- System ---
      SettingInfo::Enum(StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeout,
                        {StrId::STR_MIN_1, StrId::STR_MIN_5, StrId::STR_MIN_10, StrId::STR_MIN_15, StrId::STR_MIN_30},
                        "sleepTimeout", StrId::STR_CAT_SYSTEM),

      // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
      SettingInfo::DynamicString(
          StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
          [](const std::string& v) {
            KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
            KOREADER_STORE.saveToFile();
          },
          "koUsername", StrId::STR_KOREADER_SYNC),
      SettingInfo::DynamicString(
          StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
          [](const std::string& v) {
            KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
            KOREADER_STORE.saveToFile();
          },
          "koPassword", StrId::STR_KOREADER_SYNC),
      SettingInfo::DynamicString(
          StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
          [](const std::string& v) {
            KOREADER_STORE.setServerUrl(v);
            KOREADER_STORE.saveToFile();
          },
          "koServerUrl", StrId::STR_KOREADER_SYNC),
      SettingInfo::DynamicEnum(
          StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
          [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
          [](uint8_t v) {
            KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
            KOREADER_STORE.saveToFile();
          },
          "koMatchMethod", StrId::STR_KOREADER_SYNC),

      // --- OPDS Browser (web-only, uses CrossPointSettings char arrays) ---
      SettingInfo::String(StrId::STR_OPDS_SERVER_URL, SETTINGS.opdsServerUrl, sizeof(SETTINGS.opdsServerUrl),
                          "opdsServerUrl", StrId::STR_OPDS_BROWSER),
      SettingInfo::String(StrId::STR_USERNAME, SETTINGS.opdsUsername, sizeof(SETTINGS.opdsUsername), "opdsUsername",
                          StrId::STR_OPDS_BROWSER),
      SettingInfo::String(StrId::STR_PASSWORD, SETTINGS.opdsPassword, sizeof(SETTINGS.opdsPassword), "opdsPassword",
                          StrId::STR_OPDS_BROWSER),
  };
}