#pragma once

#include <I18n.h>

#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "activities/settings/SettingInfo.h"

// Shared settings list used by both the device settings UI and the web settings API.
//
// Fields that drive UI behaviour:
//   category    — which tab the setting appears under (STR_CAT_DISPLAY, STR_CAT_READER, …).
//                 Entries with STR_NONE_OPT or web-only categories are skipped by the device UI.
//   subcategory — optional section heading within a tab.  Items remain in their defined order;
//                 no reordering or grouping occurs.  When an item's subcategory differs from the
//                 previous item's, SettingsActivity::onEnter() automatically inserts a separator
//                 row before it.  Add with .withSubcategory(StrId::STR_MY_SECTION).
//                 Items without a subcategory (STR_NONE_OPT) never trigger a separator.
//   submenu     — optional submenu grouping.  Items with the same submenu StrId are hidden from
//                 the main list and collected behind a single placeholder entry.  Selecting that
//                 entry launches SettingsSubmenuActivity with those items.  withSubcategory()
//                 works inside a submenu exactly as it does in the parent tab.
//                 Add with .withSubmenu(StrId::STR_MY_SUBMENU).
//   key         — JSON property name used by the web settings API (nullptr = device-only).
//
// ACTION-type entries and entries without a key are device-only and are added directly
// in SettingsActivity::onEnter(), not here.
//
// Implementation note: the list is a namespace-level static (not a function-local static) so
// it is initialised during the global-static phase before setup() runs.  A function-local
// static would trigger __cxa_guard_acquire on the first call, which creates a FreeRTOS mutex
// deep inside the heap allocator chain — enough stack to overflow the 8 KB loop task stack
// when called from inside SETTINGS.loadFromFile() at boot time.
namespace SettingsListDetail {
inline const std::vector<SettingInfo> list = {
    // --- Display ---
    SettingInfo::Enum(StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeout,
                      {StrId::STR_MIN_1, StrId::STR_MIN_5, StrId::STR_MIN_10, StrId::STR_MIN_15, StrId::STR_MIN_30},
                      "sleepTimeout", StrId::STR_CAT_DISPLAY),
    SettingInfo::Enum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
                      {StrId::STR_DARK, StrId::STR_LIGHT, StrId::STR_CUSTOM, StrId::STR_COVER, StrId::STR_NONE_OPT,
                       StrId::STR_COVER_CUSTOM, StrId::STR_PAGE_OVERLAY},
                      "sleepScreen", StrId::STR_CAT_DISPLAY)
        .withSubcategory(StrId::STR_MENU_DISP_SLEEP),
    SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                      {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY),
    SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                      {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED}, "sleepScreenCoverFilter",
                      StrId::STR_CAT_DISPLAY),
    SettingInfo::Enum(
        StrId::STR_SLEEP_COVER_OVERLAY, &CrossPointSettings::sleepCoverOverlay,
        {StrId::STR_OVERLAY_OFF, StrId::STR_OVERLAY_WHITE, StrId::STR_OVERLAY_GRAY, StrId::STR_OVERLAY_BLACK},
        "sleepCoverOverlay", StrId::STR_CAT_DISPLAY),
    SettingInfo::Enum(StrId::STR_SLEEP_IMAGE_PICK_MODE, &CrossPointSettings::sleepImagePickMode,
                      {StrId::STR_RANDOM, StrId::STR_SEQUENTIAL}, "sleepImagePickMode", StrId::STR_CAT_DISPLAY),
    SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                      {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                      StrId::STR_CAT_DISPLAY)
        .withSubcategory(StrId::STR_MENU_DISP_BATTERY),
    SettingInfo::Enum(
        StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
        {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15, StrId::STR_PAGES_30},
        "refreshFrequency", StrId::STR_CAT_DISPLAY)
        .withSubcategory(StrId::STR_MENU_DISP_REFRESH),
    SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                        StrId::STR_CAT_DISPLAY),
    SettingInfo::Enum(StrId::STR_UI_THEME, &CrossPointSettings::uiTheme,
                      {StrId::STR_THEME_CLASSIC, StrId::STR_THEME_LYRA, StrId::STR_THEME_LYRA_EXTENDED}, "uiTheme",
                      StrId::STR_CAT_DISPLAY),

    // --- Reader ---
    // General reader settings
    SettingInfo::Enum(StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
                      {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW},
                      "orientation", StrId::STR_CAT_READER),
    // Font
    SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily,
                      {StrId::STR_BOOKERLY, StrId::STR_NOTO_SANS, StrId::STR_OPEN_DYSLEXIC}, "fontFamily",
                      StrId::STR_CAT_READER)
        .withSubcategory(StrId::STR_MENU_READER_FONT),
    SettingInfo::Enum(StrId::STR_FONT_SIZE, &CrossPointSettings::fontSize,
                      {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE}, "fontSize",
                      StrId::STR_CAT_READER)
        .withSubmenu(StrId::STR_MENU_READER_FONT_SETTINGS),
    SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                        StrId::STR_CAT_READER)
        .withSubmenu(StrId::STR_MENU_READER_FONT_SETTINGS),
    SettingInfo::Enum(StrId::STR_TEXT_DARKNESS, &CrossPointSettings::textDarkness,
                      {StrId::STR_NORMAL, StrId::STR_DARK, StrId::STR_EXTRA_DARK, StrId::STR_MAX_DARK}, "textDarkness",
                      StrId::STR_CAT_READER)
        .withSubmenu(StrId::STR_MENU_READER_FONT_SETTINGS),

    // Formatting settings
    SettingInfo::Enum(
        StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
        {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE},
        "paragraphAlignment", StrId::STR_CAT_READER)
        .withSubcategory(StrId::STR_MENU_READER_LAYOUT),
    SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                        StrId::STR_CAT_READER),
    SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled",
                        StrId::STR_CAT_READER),
    SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                      {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                      "imageRendering", StrId::STR_CAT_READER),
    SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin, {5, 40, 5}, "screenMargin",
                       StrId::STR_CAT_READER)
        .withSubmenu(StrId::STR_MENU_READER_SPACING),
    SettingInfo::Enum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing,
                      {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE}, "lineSpacing", StrId::STR_CAT_READER)
        .withSubmenu(StrId::STR_MENU_READER_SPACING),
    SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing, "extraParagraphSpacing",
                        StrId::STR_CAT_READER)
        .withSubmenu(StrId::STR_MENU_READER_SPACING),
    // Generic reader settings
    SettingInfo::Toggle(StrId::STR_CREATE_FALLBACK_FOR_INVALID_TOC, &CrossPointSettings::syntheticTocFallback,
                        "syntheticTocFallback", StrId::STR_CAT_READER)
        .withSubcategory(StrId::STR_MENU_READER_TWEAKS),

    // --- Controls ---
    SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                      {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV}, "sideButtonLayout", StrId::STR_CAT_CONTROLS),
    SettingInfo::Toggle(StrId::STR_LONG_PRESS_SKIP, &CrossPointSettings::longPressChapterSkip, "longPressChapterSkip",
                        StrId::STR_CAT_CONTROLS),
    SettingInfo::Enum(StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
                      {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_PAGE_TURN, StrId::STR_FORCE_REFRESH,
                       StrId::STR_FOOTNOTES, StrId::STR_STAR_PAGE},
                      "shortPwrBtn", StrId::STR_CAT_CONTROLS),

    // --- System ---
    SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles, "showHiddenFiles",
                        StrId::STR_CAT_SYSTEM),
    SettingInfo::Toggle(StrId::STR_SHOW_FILE_EXTENSIONS, &CrossPointSettings::showFileExtensions, "showFileExtensions",
                        StrId::STR_CAT_SYSTEM),

    // Will be dealt with separately , so do receive none of the main categories to be visible in the web UI but not the
    // device UI
    SettingInfo::Toggle(StrId::STR_USE_CLOCK, &CrossPointSettings::useClock, "useClock", StrId::STR_CLOCK),
    SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat12h, {StrId::STR_24H, StrId::STR_12H},
                      "clockFormat12h", StrId::STR_CLOCK),
    SettingInfo::Enum(StrId::STR_TIMEZONE, &CrossPointSettings::timeZone,
                      {StrId::STR_TZ_UTC, StrId::STR_TZ_CET, StrId::STR_TZ_EET, StrId::STR_TZ_MSK,
                       StrId::STR_TZ_UTC_PLUS4, StrId::STR_TZ_IST, StrId::STR_TZ_UTC_PLUS7, StrId::STR_TZ_UTC_PLUS8,
                       StrId::STR_TZ_UTC_PLUS9, StrId::STR_TZ_AEST, StrId::STR_TZ_NZST, StrId::STR_TZ_UTC_MINUS3,
                       StrId::STR_TZ_EST, StrId::STR_TZ_CST, StrId::STR_TZ_MST, StrId::STR_TZ_PST},
                      "timeZone", StrId::STR_CLOCK),
    // Weather
    SettingInfo::Toggle(StrId::STR_USE_WEATHER, &CrossPointSettings::useWeather, "useWeather", StrId::STR_WEATHER),

    // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
    SettingInfo::DynamicString(
        StrId::STR_SYNC_SERVER_URL, [](void*) { return KOREADER_STORE.getServerUrl(); },
        [](void*, const std::string& v) {
          KOREADER_STORE.setServerUrl(v);
          KOREADER_STORE.saveToFile();
        },
        "koServerUrl", StrId::STR_KOREADER_SYNC),
    SettingInfo::DynamicString(
        StrId::STR_KOREADER_USERNAME, [](void*) { return KOREADER_STORE.getUsername(); },
        [](void*, const std::string& v) {
          KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
          KOREADER_STORE.saveToFile();
        },
        "koUsername", StrId::STR_KOREADER_SYNC),
    SettingInfo::DynamicString(
        StrId::STR_KOREADER_PASSWORD, [](void*) { return KOREADER_STORE.getPassword(); },
        [](void*, const std::string& v) {
          KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
          KOREADER_STORE.saveToFile();
        },
        "koPassword", StrId::STR_KOREADER_SYNC)
        .withObfuscated(),
    SettingInfo::DynamicEnum(
        StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
        [](void*) { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
        [](void*, uint8_t v) {
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
                        StrId::STR_OPDS_BROWSER)
        .withObfuscated(),

    // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
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
    SettingInfo::Toggle(StrId::STR_CLOCK, &CrossPointSettings::statusBarClock, "statusBarClock",
                        StrId::STR_CUSTOMISE_STATUS_BAR),
};
}  // namespace SettingsListDetail

inline const std::vector<SettingInfo>& getSettingsList() { return SettingsListDetail::list; }
