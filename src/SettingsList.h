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
      SettingInfo::Enum(StrId::SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
                        {StrId::DARK, StrId::LIGHT, StrId::CUSTOM, StrId::COVER, StrId::NONE_OPT, StrId::COVER_CUSTOM},
                        "sleepScreen", StrId::CAT_DISPLAY),
      SettingInfo::Enum(StrId::SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode, {StrId::FIT, StrId::CROP},
                        "sleepScreenCoverMode", StrId::CAT_DISPLAY),
      SettingInfo::Enum(StrId::SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                        {StrId::NONE_OPT, StrId::FILTER_CONTRAST, StrId::INVERTED}, "sleepScreenCoverFilter",
                        StrId::CAT_DISPLAY),
      SettingInfo::Enum(StrId::STATUS_BAR, &CrossPointSettings::statusBar,
                        {StrId::NONE_OPT, StrId::NO_PROGRESS, StrId::STATUS_BAR_FULL_PERCENT,
                         StrId::STATUS_BAR_FULL_BOOK, StrId::STATUS_BAR_BOOK_ONLY, StrId::STATUS_BAR_FULL_CHAPTER},
                        "statusBar", StrId::CAT_DISPLAY),
      SettingInfo::Enum(StrId::HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                        {StrId::NEVER, StrId::IN_READER, StrId::ALWAYS}, "hideBatteryPercentage", StrId::CAT_DISPLAY),
      SettingInfo::Enum(StrId::REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
                        {StrId::PAGES_1, StrId::PAGES_5, StrId::PAGES_10, StrId::PAGES_15, StrId::PAGES_30},
                        "refreshFrequency", StrId::CAT_DISPLAY),
      SettingInfo::Enum(StrId::UI_THEME, &CrossPointSettings::uiTheme, {StrId::THEME_CLASSIC, StrId::THEME_LYRA},
                        "uiTheme", StrId::CAT_DISPLAY),
      SettingInfo::Toggle(StrId::SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix", StrId::CAT_DISPLAY),

      // --- Reader ---
      SettingInfo::Enum(StrId::FONT_FAMILY, &CrossPointSettings::fontFamily,
                        {StrId::BOOKERLY, StrId::NOTO_SANS, StrId::OPEN_DYSLEXIC}, "fontFamily", StrId::CAT_READER),
      SettingInfo::Enum(StrId::FONT_SIZE, &CrossPointSettings::fontSize,
                        {StrId::SMALL, StrId::MEDIUM, StrId::LARGE, StrId::X_LARGE}, "fontSize", StrId::CAT_READER),
      SettingInfo::Enum(StrId::LINE_SPACING, &CrossPointSettings::lineSpacing,
                        {StrId::TIGHT, StrId::NORMAL, StrId::WIDE}, "lineSpacing", StrId::CAT_READER),
      SettingInfo::Value(StrId::SCREEN_MARGIN, &CrossPointSettings::screenMargin, {5, 40, 5}, "screenMargin",
                         StrId::CAT_READER),
      SettingInfo::Enum(StrId::PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                        {StrId::JUSTIFY, StrId::ALIGN_LEFT, StrId::CENTER, StrId::ALIGN_RIGHT, StrId::BOOK_S_STYLE},
                        "paragraphAlignment", StrId::CAT_READER),
      SettingInfo::Toggle(StrId::EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                          StrId::CAT_READER),
      SettingInfo::Toggle(StrId::HYPHENATION, &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled",
                          StrId::CAT_READER),
      SettingInfo::Enum(StrId::ORIENTATION, &CrossPointSettings::orientation,
                        {StrId::PORTRAIT, StrId::LANDSCAPE_CW, StrId::INVERTED, StrId::LANDSCAPE_CCW}, "orientation",
                        StrId::CAT_READER),
      SettingInfo::Toggle(StrId::EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing, "extraParagraphSpacing",
                          StrId::CAT_READER),
      SettingInfo::Toggle(StrId::TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing", StrId::CAT_READER),

      // --- Controls ---
      SettingInfo::Enum(StrId::SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                        {StrId::PREV_NEXT, StrId::NEXT_PREV}, "sideButtonLayout", StrId::CAT_CONTROLS),
      SettingInfo::Toggle(StrId::LONG_PRESS_SKIP, &CrossPointSettings::longPressChapterSkip, "longPressChapterSkip",
                          StrId::CAT_CONTROLS),
      SettingInfo::Enum(StrId::SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
                        {StrId::IGNORE, StrId::SLEEP, StrId::PAGE_TURN}, "shortPwrBtn", StrId::CAT_CONTROLS),

      // --- System ---
      SettingInfo::Enum(StrId::TIME_TO_SLEEP, &CrossPointSettings::sleepTimeout,
                        {StrId::MIN_1, StrId::MIN_5, StrId::MIN_10, StrId::MIN_15, StrId::MIN_30}, "sleepTimeout",
                        StrId::CAT_SYSTEM),

      // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
      SettingInfo::DynamicString(
          StrId::KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
          [](const std::string& v) {
            KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
            KOREADER_STORE.saveToFile();
          },
          "koUsername", StrId::KOREADER_SYNC),
      SettingInfo::DynamicString(
          StrId::KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
          [](const std::string& v) {
            KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
            KOREADER_STORE.saveToFile();
          },
          "koPassword", StrId::KOREADER_SYNC),
      SettingInfo::DynamicString(
          StrId::SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
          [](const std::string& v) {
            KOREADER_STORE.setServerUrl(v);
            KOREADER_STORE.saveToFile();
          },
          "koServerUrl", StrId::KOREADER_SYNC),
      SettingInfo::DynamicEnum(
          StrId::DOCUMENT_MATCHING, {StrId::FILENAME, StrId::BINARY},
          [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
          [](uint8_t v) {
            KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
            KOREADER_STORE.saveToFile();
          },
          "koMatchMethod", StrId::KOREADER_SYNC),

      // --- OPDS Browser (web-only, uses CrossPointSettings char arrays) ---
      SettingInfo::String(StrId::OPDS_SERVER_URL, SETTINGS.opdsServerUrl, sizeof(SETTINGS.opdsServerUrl),
                          "opdsServerUrl", StrId::OPDS_BROWSER),
      SettingInfo::String(StrId::USERNAME, SETTINGS.opdsUsername, sizeof(SETTINGS.opdsUsername), "opdsUsername",
                          StrId::OPDS_BROWSER),
      SettingInfo::String(StrId::PASSWORD, SETTINGS.opdsPassword, sizeof(SETTINGS.opdsPassword), "opdsPassword",
                          StrId::OPDS_BROWSER),
  };
}