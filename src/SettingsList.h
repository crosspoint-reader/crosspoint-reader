#pragma once

#include <I18n.h>
#include <SdCardFontRegistry.h>

#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "activities/settings/SettingsActivity.h"

// Number of built-in font families (Bookerly, NotoSans, OpenDyslexic)
static constexpr uint8_t BUILTIN_FONT_COUNT = 3;

// Build the font family setting dynamically. When registry is non-null, SD card fonts
// are appended after the built-in fonts. Otherwise only built-in fonts are listed.
inline SettingInfo buildFontFamilySetting(const SdCardFontRegistry* registry) {
  // Built-in font labels (StrId)
  std::vector<StrId> enumValues = {StrId::STR_BOOKERLY, StrId::STR_NOTO_SANS, StrId::STR_OPEN_DYSLEXIC};
  // Runtime string labels for SD card fonts
  std::vector<std::string> enumStringValues;

  // Reserve: first BUILTIN_FONT_COUNT entries use StrId, rest use strings
  if (registry) {
    for (const auto& family : registry->getFamilies()) {
      enumStringValues.push_back(family.name);
    }
  }

  // Capture the SD font count for the lambdas
  const int sdFontCount = static_cast<int>(enumStringValues.size());

  // Total option count = built-in + SD card families
  // For the combined enumStringValues: we need all entries as strings (built-in names + SD names)
  // The render code checks enumStringValues first, then enumValues. So we build enumStringValues
  // with all options when SD fonts are present.
  std::vector<std::string> allStringValues;
  if (sdFontCount > 0) {
    allStringValues.push_back(I18N.get(StrId::STR_BOOKERLY));
    allStringValues.push_back(I18N.get(StrId::STR_NOTO_SANS));
    allStringValues.push_back(I18N.get(StrId::STR_OPEN_DYSLEXIC));
    for (auto& name : enumStringValues) {
      allStringValues.push_back(name);
    }
  }

  SettingInfo s;
  s.nameId = StrId::STR_FONT_FAMILY;
  s.type = SettingType::ENUM;
  s.enumValues = std::move(enumValues);
  s.enumStringValues = std::move(allStringValues);
  s.key = "fontFamily";
  s.category = StrId::STR_CAT_READER;

  // Capture registry families by copy for the lambdas
  std::vector<std::string> sdFamilyNames;
  if (registry) {
    for (const auto& f : registry->getFamilies()) {
      sdFamilyNames.push_back(f.name);
    }
  }

  s.valueGetter = [sdFamilyNames]() -> uint8_t {
    // If an SD card font is selected, find its index
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      for (int i = 0; i < static_cast<int>(sdFamilyNames.size()); i++) {
        if (sdFamilyNames[i] == SETTINGS.sdFontFamilyName) {
          return static_cast<uint8_t>(BUILTIN_FONT_COUNT + i);
        }
      }
      // SD font name not found in registry — fall through to built-in
    }
    return SETTINGS.fontFamily < BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
  };

  s.valueSetter = [sdFamilyNames](uint8_t v) {
    if (v < BUILTIN_FONT_COUNT) {
      SETTINGS.fontFamily = v;
      SETTINGS.sdFontFamilyName[0] = '\0';
    } else {
      int sdIdx = v - BUILTIN_FONT_COUNT;
      if (sdIdx >= 0 && sdIdx < static_cast<int>(sdFamilyNames.size())) {
        strncpy(SETTINGS.sdFontFamilyName, sdFamilyNames[sdIdx].c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
        SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      }
    }
  };

  return s;
}

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
// Pass registry to include SD card fonts in the font family setting.
inline std::vector<SettingInfo> getSettingsList(const SdCardFontRegistry* registry = nullptr) {
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
      buildFontFamilySetting(registry),
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
      SettingInfo::Toggle(StrId::STR_LONG_PRESS_SKIP, &CrossPointSettings::longPressChapterSkip, "longPressChapterSkip",
                          StrId::STR_CAT_CONTROLS),
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