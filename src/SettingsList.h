#pragma once

#include <BoardConfig.h>
#include <HalCapabilities.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iterator>
#include <string>
#include <type_traits>
#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "SdCardFontGlobals.h"
#include "TouchGestures.h"
#include "TouchUi.h"
#include "activities/reader/KOReaderAutoSync.h"
#include "activities/settings/SettingInfo.h"
#include "activities/settings/TimezoneOptions.h"

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
//   requiredCapability — hardware visibility. Default is "always visible";
//     override with .requiring(SettingRequires::X) to hide the setting on boards
//     that physically cannot do the thing it controls. Ask for a CAPABILITY,
//     never a board name — see SettingInfo.h.
//
// ACTION-type entries and entries without a key are device-only and are added directly
// in SettingsActivity::onEnter(), not here.
//
// Implementation note: the rows are built at runtime, one at a time, by forEachSetting(), and
// never held as a list (see there for why). Not a static list either: a function-local static
// would trigger __cxa_guard_acquire on first call, which creates a FreeRTOS mutex deep inside
// the heap allocator chain — enough stack to overflow the 8 KB loop task stack when called from
// inside SETTINGS.loadFromFile() at boot time.
namespace SettingsListDetail {
inline uint8_t getKoReaderMatchMethod(const void*) { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); }

inline std::string getKoReaderServerUrl(void*) { return KOREADER_STORE.getServerUrl(); }

inline std::string getKoReaderUsername(void*) { return KOREADER_STORE.getUsername(); }

inline std::string getKoReaderPassword(void*) { return KOREADER_STORE.getPassword(); }

inline std::string getSleepTimeoutDisplay(void*) {
  const uint8_t v = SETTINGS.sleepTimeoutMinutes;
  if (v == 0) return std::string(tr(STR_NEVER));
  return std::to_string(v) + tr(STR_MIN_SUFFIX);
}

inline std::string getDictionaryDisplay(void*) {
  if (SETTINGS.dictionaryName[0] == '\0') return std::string(tr(STR_NONE_OPT));
  return SETTINGS.dictionaryName;
}

inline std::string getRefreshFrequencyDisplay(void*) {
  const uint8_t v = SETTINGS.refreshFrequencyPages;
  if (v == 0) return std::string(tr(STR_NEVER));
  return std::to_string(v) + tr(STR_PAGES_SUFFIX);
}

inline std::string getFrontlightBrightnessDisplay(void*) { return std::to_string(SETTINGS.frontlightBrightness) + "%"; }

inline std::string getFrontlightWarmthDisplay(void*) { return std::to_string(SETTINGS.frontlightWarmth) + "%"; }

// The visitor forEachSetting() hands the builder below, behind a plain function pointer. As a
// template parameter it compiled the whole ~115-row builder once per visitor, seven copies of
// it in all; type-erased, there is one, and each visitor adds only the thunk that calls it.
struct RowSink {
  void* visitor;
  void (*take)(void* visitor, SettingInfo& row);
  // A row is a temporary, or a builder chain (withSubcategory() and the like) that returns an
  // lvalue reference to one. Either lives until the call returns.
  void operator()(SettingInfo& row) const { take(visitor, row); }
  void operator()(SettingInfo&& row) const { take(visitor, row); }
};

// Every settings row in order, built one at a time and handed to `emit`. Nothing here holds the
// list: see forEachSetting().
inline void buildSettings(const RowSink& emit) {
  // Shared button action options used by all button enum entries.
  const std::vector<StrId> btnActionOptions = {StrId::STR_BTN_ACT_PAGE_FORWARD,
                                               StrId::STR_BTN_ACT_PAGE_BACK,
                                               StrId::STR_BTN_ACT_PAGE_FORWARD_10,
                                               StrId::STR_BTN_ACT_PAGE_BACK_10,
                                               StrId::STR_BTN_ACT_GO_HOME,
                                               StrId::STR_BTN_ACT_SLEEP,
                                               StrId::STR_BTN_ACT_FORCE_REFRESH,
                                               StrId::STR_BTN_ACT_FORCE_FAST_REFRESH,
                                               StrId::STR_BTN_ACT_OPEN_TOC,
                                               StrId::STR_BTN_ACT_OPEN_BOOKMARKS,
                                               StrId::STR_BTN_ACT_STAR_PAGE,
                                               StrId::STR_BTN_ACT_FOOTNOTES,
                                               StrId::STR_BTN_ACT_NEXT_SECTION,
                                               StrId::STR_BTN_ACT_PREV_SECTION,
                                               StrId::STR_BTN_ACT_EXIT_READER,
                                               StrId::STR_BTN_ACT_READER_MENU,
                                               StrId::STR_BTN_ACT_TOGGLE_BIONIC_READING,
                                               StrId::STR_BTN_ACT_KOREADER_SYNC,
                                               StrId::STR_BTN_ACT_CYCLE_FONT_SIZE,
                                               StrId::STR_BTN_ACT_CYCLE_ORIENTATION,
                                               StrId::STR_BTN_ACT_QUICK_OVERRIDES,
                                               StrId::STR_BTN_ACT_IGNORE,
                                               StrId::STR_DICTIONARY};

  // Offered everywhere, so a plain continuation of btnActionOptions above. Kept
  // separate only for legibility.
  const std::vector<StrId> extraActionOptions = {
      StrId::STR_BTN_ACT_FONT_SIZE_SMALLER, StrId::STR_BTN_ACT_FONT_SIZE_LARGER, StrId::STR_BTN_ACT_TOGGLE_TOUCH_UI,
      StrId::STR_BTN_ACT_CYCLE_ORIENTATION_BACK};
  // The ONLY conditional block, and correspondingly the LAST entries of
  // BUTTON_ACTION. Dropping them on a board with no light therefore leaves every
  // other action's numeric value untouched, so a settings file written on a lit
  // board still reads correctly here. Anything added after these would break
  // that — see the comment on BUTTON_ACTION.
  const std::vector<StrId> lightActionOptions = {StrId::STR_BTN_ACT_LIGHT_TOGGLE, StrId::STR_BTN_ACT_LIGHT_BRIGHTER,
                                                 StrId::STR_BTN_ACT_LIGHT_DIMMER};
  // Narrower still, and correspondingly last: a warm/cool pair is a subset of
  // the lit boards. Dropping these on a single-channel light leaves every value
  // above untouched.
  const std::vector<StrId> warmActionOptions = {StrId::STR_BTN_ACT_LIGHT_WARMER, StrId::STR_BTN_ACT_LIGHT_COOLER};

  // Prepend the per-button default action to the shared options list.
  // The option list is positional — index i is BUTTON_ACTION value i — so it must
  // name every action exactly once. Asserted because the failure mode is not a
  // crash but a silent misreading of every stored button and gesture mapping,
  // which would look like the settings screen forgetting things at random.
  assert(1 + btnActionOptions.size() + extraActionOptions.size() + lightActionOptions.size() +
                 warmActionOptions.size() ==
             static_cast<size_t>(CrossPointSettings::BUTTON_ACTION_COUNT) &&
         "btnActionOptions must name every BUTTON_ACTION except BTN_DEFAULT, in enum order");

  const bool hasLight = Frontlight.present();
  const bool hasWarmLight = hasLight && Frontlight.hasColorTemperature();
  auto makeBtnActionOptions = [&](StrId defaultAction) {
    std::vector<StrId> result;
    // Reserves for the light options whether or not this board has them: a hint
    // does not need to be exact, and three spare pointers cost less than a
    // condition that reads as dead code on every build without a frontlight.
    result.reserve(1 + btnActionOptions.size() + extraActionOptions.size() + lightActionOptions.size() +
                   warmActionOptions.size());
    result.push_back(defaultAction);
    result.insert(result.end(), btnActionOptions.begin(), btnActionOptions.end());
    result.insert(result.end(), extraActionOptions.begin(), extraActionOptions.end());
    // cppcheck-suppress knownConditionTrueFalse ; hasLight is a compile-time false
    // on a build with FREEINK_CAP_FRONTLIGHT off, where present() returns a literal.
    // It is a genuine runtime question on every board that has a light.
    if (hasLight) {
      result.insert(result.end(), lightActionOptions.begin(), lightActionOptions.end());
    }
    // cppcheck-suppress knownConditionTrueFalse ; see the hasLight check above.
    if (hasWarmLight) {
      result.insert(result.end(), warmActionOptions.begin(), warmActionOptions.end());
    }
    return result;
  };

  // --- Display ---
  emit(SettingInfo::Action(StrId::STR_TIME_TO_SLEEP, SettingAction::SleepTimeoutPicker)
           .persisting(&CrossPointSettings::sleepTimeoutMinutes, "sleepTimeoutMinutes", 60)
           .withDisplayGetter(getSleepTimeoutDisplay)
           .withCategory(StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
                         {StrId::STR_DARK, StrId::STR_LIGHT, StrId::STR_CUSTOM, StrId::STR_COVER, StrId::STR_NONE_OPT,
                          StrId::STR_COVER_CUSTOM, StrId::STR_PAGE_OVERLAY, StrId::STR_QUICK_RESUME},
                         "sleepScreen", StrId::STR_CAT_DISPLAY)
           .withSubcategory(StrId::STR_MENU_DISP_SLEEP)
           .withSelectorActivity());
  // Extra clearance from the panel edge, on top of what the board profile declares. Display
  // rather than Reader because it moves the status bar too, and because it is a property of the
  // device's case rather than of how one likes text laid out -- see CrossPointSettings::edgeMargin.
  emit(SettingInfo::Enum(StrId::STR_EDGE_MARGIN, &CrossPointSettings::edgeMargin,
                         {StrId::STR_NARROW, StrId::STR_MEDIUM, StrId::STR_LARGE}, "edgeMargin",
                         StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                         {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                         {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED,
                          StrId::STR_FILTER_ADAPTIVE, StrId::STR_FILTER_EQUALIZE},
                         "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(
           StrId::STR_SLEEP_COVER_OVERLAY, &CrossPointSettings::sleepCoverOverlay,
           {StrId::STR_OVERLAY_OFF, StrId::STR_OVERLAY_WHITE, StrId::STR_OVERLAY_GRAY, StrId::STR_OVERLAY_BLACK},
           "sleepCoverOverlay", StrId::STR_CAT_DISPLAY)
           .withSelectorActivity());
  emit(SettingInfo::Enum(StrId::STR_SLEEP_IMAGE_PICK_MODE, &CrossPointSettings::sleepImagePickMode,
                         {StrId::STR_RANDOM, StrId::STR_SEQUENTIAL}, "sleepImagePickMode", StrId::STR_CAT_DISPLAY));
  // A TOGGLE, not a two-option ENUM over Off/On -- which is what this used to be. The field is
  // QUICK_RESUME_SLEEP_SCREEN (NEVER=0 / AFTER_TIMEOUT=1) and every consumer compares it
  // numerically, so nothing about the stored value changes: JsonSettingsIO clamps an ENUM to
  // enumValues.size() and a TOGGLE to 2, which for this row is the same bound. What does change is
  // that the row now draws as a switch like every other on/off setting, and the web API reports it
  // as "toggle" so the browser renders a checkbox rather than a two-item dropdown.
  emit(SettingInfo::Toggle(StrId::STR_QUICK_RESUME_TIMEOUT, &CrossPointSettings::quickResumeSleepScreen,
                           "quickResumeSleepScreen", StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                         {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                         StrId::STR_CAT_DISPLAY)
           .withSubcategory(StrId::STR_MENU_DISP_BATTERY));
  // --- User interface ---
  // One heading for the four rows: headings stick, so the rows after UI Theme sit under it.
  emit(SettingInfo::Enum(StrId::STR_UI_THEME, &CrossPointSettings::uiTheme,
                         {StrId::STR_THEME_CLASSIC, StrId::STR_THEME_LYRA, StrId::STR_THEME_LYRA_EXTENDED,
                          StrId::STR_THEME_LYRA_CAROUSEL},
                         "uiTheme", StrId::STR_CAT_DISPLAY)
           .withSelectorActivity()
           .withSubcategory(StrId::STR_MENU_DISP_UI));
  emit(SettingInfo::Enum(StrId::STR_UI_FONT_SIZE, &CrossPointSettings::uiFontSize,
                         {StrId::STR_NORMAL, StrId::STR_LARGE}, "uiFontSize", StrId::STR_CAT_DISPLAY));
  // Home screen entries: on = on the home screen, off = behind its "More" entry.
  emit(SettingInfo::Toggle(StrId::STR_LIBRARY, &CrossPointSettings::showLibraryOnHome, "showLibraryOnHome",
                           StrId::STR_CAT_DISPLAY)
           .withSubmenu(StrId::STR_MENU_DISP_HOME));
  emit(SettingInfo::Toggle(StrId::STR_READING_STATS, &CrossPointSettings::showReadingStatsOnHome,
                           "showReadingStatsOnHome", StrId::STR_CAT_DISPLAY)
           .withSubmenu(StrId::STR_MENU_DISP_HOME));
  emit(SettingInfo::Toggle(StrId::STR_GLOBAL_BOOKMARKS, &CrossPointSettings::showBookmarksOnHome, "showBookmarksOnHome",
                           StrId::STR_CAT_DISPLAY)
           .withSubmenu(StrId::STR_MENU_DISP_HOME));
  emit(SettingInfo::Toggle(StrId::STR_FILE_TRANSFER, &CrossPointSettings::showFileTransferOnHome,
                           "showFileTransferOnHome", StrId::STR_CAT_DISPLAY)
           .withSubmenu(StrId::STR_MENU_DISP_HOME));
  emit(SettingInfo::Toggle(StrId::STR_WEATHER, &CrossPointSettings::showWeatherOnHome, "showWeatherOnHome",
                           StrId::STR_CAT_DISPLAY)
           .withSubmenu(StrId::STR_MENU_DISP_HOME));
  emit(SettingInfo::Toggle(StrId::STR_SHOW_BUSY_INDICATOR, &CrossPointSettings::showBusyIndicator, "showBusyIndicator",
                           StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Action(StrId::STR_REFRESH_FREQ, SettingAction::RefreshFrequencyPicker)
           .persisting(&CrossPointSettings::refreshFrequencyPages, "refreshFrequencyPages", 60)
           .withDisplayGetter(getRefreshFrequencyDisplay)
           .withCategory(StrId::STR_CAT_DISPLAY)
           .withSubmenu(StrId::STR_MENU_DISP_REFRESH)
           .withSubcategory(StrId::STR_MENU_DISP_REFRESH));
  emit(SettingInfo::Toggle(StrId::STR_REFRESH_AFTER_IMAGE_PAGES, &CrossPointSettings::halfRefreshAfterImagePage,
                           "halfRefreshAfterImagePage", StrId::STR_CAT_DISPLAY)
           .withSubmenu(StrId::STR_MENU_DISP_REFRESH)
           .withSubcategory(StrId::STR_MENU_DISP_REFRESH));
  // Offered only where the panel actually fades. It is not free when enabled: it forces
  // turnOffScreen on every refresh on every path, which costs panel power-sequencing time per
  // page. A board whose glass does not fade would pay that for nothing.
  emit(SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                           StrId::STR_CAT_DISPLAY)
           .requiring(SettingRequires::SunlightFadingPanel));

  // --- Reading light (boards with a PWM frontlight/backlight) ---
  // Grouped in a submenu so the Display tab stays one screen on boards that
  // have one, and disappears entirely on boards that do not.
  emit(SettingInfo::DynamicToggle(
           StrId::STR_READING_LIGHT, [](const void*) -> uint8_t { return Frontlight.isOn() ? 1 : 0; },
           [](void*, const uint8_t v) {
             // The hardware, not SETTINGS, is the authority for
             // "is the light on" (a wake with Restore off leaves
             // them legitimately apart), so read it back rather
             // than mirroring the requested value.
             Frontlight.setOn(v != 0);
             SETTINGS.frontlightOn = Frontlight.isOn() ? 1 : 0;
           },
           "frontlightOn", StrId::STR_CAT_DISPLAY)
           .withSubmenu(StrId::STR_MENU_DISP_LIGHT)
           .withSubcategory(StrId::STR_MENU_DISP_LIGHT)
           .requiring(SettingRequires::ReadingLight));
  emit(SettingInfo::Action(StrId::STR_LIGHT_BRIGHTNESS, SettingAction::FrontlightBrightnessPicker)
           .persisting(&CrossPointSettings::frontlightBrightness, "frontlightBrightness", 100)
           .withDisplayGetter(getFrontlightBrightnessDisplay)
           .withCategory(StrId::STR_CAT_DISPLAY)
           .withSubmenu(StrId::STR_MENU_DISP_LIGHT)
           .requiring(SettingRequires::ReadingLight));
  emit(SettingInfo::Action(StrId::STR_LIGHT_WARMTH, SettingAction::FrontlightWarmthPicker)
           .persisting(&CrossPointSettings::frontlightWarmth, "frontlightWarmth", 100)
           .withDisplayGetter(getFrontlightWarmthDisplay)
           .withCategory(StrId::STR_CAT_DISPLAY)
           .withSubmenu(StrId::STR_MENU_DISP_LIGHT)
           .requiring(SettingRequires::WarmLight));
  emit(SettingInfo::Toggle(StrId::STR_RESTORE_LIGHT_ON_WAKE, &CrossPointSettings::frontlightRestoreOnWake,
                           "frontlightRestoreOnWake", StrId::STR_CAT_DISPLAY)
           .withSubmenu(StrId::STR_MENU_DISP_LIGHT)
           .requiring(SettingRequires::ReadingLight));

  // --- Reader ---
  // General reader settings
  emit(SettingInfo::Enum(StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
                         {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW},
                         "orientation", StrId::STR_CAT_READER)
           .withSelectorActivity());
  // EPUB font submenu — family first, then size/AA/darkness.
  // DynamicEnum so SD card font families can be appended at the consumer
  // side (SettingsActivity / CrossPointWebServer enrich enumLabels before
  // iterating). The built-in StrIds are kept as a fallback for code paths that
  // don't enrich enumLabels.
  emit(SettingInfo::DynamicEnum(StrId::STR_FONT_FAMILY, {StrId::STR_BOOKERLY, StrId::STR_NOTO_SANS},
                                fontFamilyDynamicGetter, fontFamilyDynamicSetter, "fontFamily", StrId::STR_CAT_READER)
           .withSubcategory(StrId::STR_MENU_READER_FONT)
           .withSubmenu(StrId::STR_MENU_READER_FONT)
           .withSelectorActivity());
  // Stored as a point size; shown as an index into the sizes the selected family offers, read
  // live so the list follows a family chosen earlier in the same submenu. The getter and setter
  // translate, and persisting() saves the point size itself under the key.
  emit(SettingInfo::DynamicEnum(StrId::STR_FONT_SIZE, {}, fontSizeDynamicGetter, fontSizeDynamicSetter, "fontSize",
                                StrId::STR_CAT_READER)
           .persisting(&CrossPointSettings::fontPointSize, "fontSize", CrossPointSettings::MAX_FONT_POINT_SIZE)
           .withDynamicOptions(fontSizeOptionCount, fontSizeOptionLabel)
           .withSubmenu(StrId::STR_MENU_READER_FONT)
           .withSelectorActivity());
  emit(SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                           StrId::STR_CAT_READER)
           .withSubmenu(StrId::STR_MENU_READER_FONT));
  // X3-only fast AA LUT toggle. Swaps the 53-frame OEM grayscale waveform
  // (~2.4 s panel time, X4-accurate grays) for the 7-frame community LUT
  // (~130 ms, mid-tones slightly darker). See freeink-sdk
  // FreeInkDisplay::setFastGrayscaleLut for trade-offs.
  emit(SettingInfo::Toggle(StrId::STR_FAST_AA, &CrossPointSettings::fastAntiAliasing, "fastAntiAliasingV2",
                           StrId::STR_CAT_READER)
           .withSubmenu(StrId::STR_MENU_READER_FONT)
           .requiring(SettingRequires::SelectableGrayscaleLut));
  emit(SettingInfo::Enum(
           StrId::STR_TEXT_DARKNESS, &CrossPointSettings::textDarkness,
           {StrId::STR_NORMAL, StrId::STR_DARK, StrId::STR_EXTRA_DARK, StrId::STR_MAX_DARK, StrId::STR_LIGHTER},
           "textDarkness", StrId::STR_CAT_READER)
           .withSubmenu(StrId::STR_MENU_READER_FONT)
           .withSelectorActivity());
  // TXT/MD font submenu — same dynamic structure as EPUB, includes SD card fonts.
  emit(SettingInfo::DynamicEnum(StrId::STR_TXT_FONT_FAMILY, {StrId::STR_BOOKERLY, StrId::STR_NOTO_SANS},
                                txtFontFamilyDynamicGetter, txtFontFamilyDynamicSetter, "txtFontFamily",
                                StrId::STR_CAT_READER)
           .withSubmenu(StrId::STR_MENU_TXT_FONT)
           .withSelectorActivity());
  emit(SettingInfo::DynamicEnum(StrId::STR_TXT_FONT_SIZE, {}, txtFontSizeDynamicGetter, txtFontSizeDynamicSetter,
                                "txtFontSize", StrId::STR_CAT_READER)
           .persisting(&CrossPointSettings::txtFontPointSize, "txtFontSize", CrossPointSettings::MAX_FONT_POINT_SIZE)
           .withDynamicOptions(txtFontSizeOptionCount, txtFontSizeOptionLabel)
           .withSubmenu(StrId::STR_MENU_TXT_FONT)
           .withSelectorActivity());
  emit(SettingInfo::Enum(StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                         {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                          StrId::STR_BOOK_S_STYLE},
                         "paragraphAlignment", StrId::STR_CAT_READER)
           .withSubcategory(StrId::STR_MENU_READER_LAYOUT)
           .withSelectorActivity());
  // Formatting settings
  emit(SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                           StrId::STR_CAT_READER));
  // A book's embedded fonts (BookFonts), in the order of CrossPointSettings::PUBLISHER_FONTS: Stage A's toggle stored
  // 0/1, which stay Off and Body Text. The setting follows crosspoint-reader PR #3934 (Justin Mitchell), reworked for
  // the TTF engine.
  emit(SettingInfo::Enum(StrId::STR_PUBLISHER_FONTS, &CrossPointSettings::publisherFonts,
                         {StrId::STR_OVERLAY_OFF, StrId::STR_PUBLISHER_FONTS_BODY, StrId::STR_PUBLISHER_FONTS_ACCENTS,
                          StrId::STR_PUBLISHER_FONTS_ALL},
                         "publisherFonts", StrId::STR_CAT_READER));
  emit(SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled",
                           StrId::STR_CAT_READER));
  emit(SettingInfo::Toggle(StrId::STR_FONT_SIZE_NORMALIZATION, &CrossPointSettings::fontSizeNormalization,
                           "fontSizeNormalization", StrId::STR_CAT_READER));
  // Which StarDict dictionary word lookup uses. An Action rather than an Enum:
  // the options are folders discovered on the SD card at open time, not a fixed
  // list, so the picker has to scan.
  emit(SettingInfo::Action(StrId::STR_DICTIONARY, SettingAction::DictionarySelect)
           .withDisplayGetter(getDictionaryDisplay)
           .withCategory(StrId::STR_CAT_READER));
  emit(SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                         {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                         "imageRendering", StrId::STR_CAT_READER)
           .withSubmenu(StrId::STR_IMAGES));
  emit(SettingInfo::Toggle(StrId::STR_LARGE_IMAGE_PLACEHOLDER, &CrossPointSettings::largeImagePlaceholder,
                           "largeImagePlaceholder", StrId::STR_CAT_READER)
           .withSubmenu(StrId::STR_IMAGES));
  emit(SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin, {5, 40, 5}, "screenMargin",
                          StrId::STR_CAT_READER)
           .withSubmenu(StrId::STR_MENU_READER_SPACING));
  emit(SettingInfo::Enum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing,
                         {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE}, "lineSpacing", StrId::STR_CAT_READER)
           .withSubmenu(StrId::STR_MENU_READER_SPACING));
  emit(SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                           "extraParagraphSpacing", StrId::STR_CAT_READER)
           .withSubmenu(StrId::STR_MENU_READER_SPACING));

  // Generic reader settings
  emit(SettingInfo::Toggle(StrId::STR_CREATE_FALLBACK_FOR_INVALID_TOC, &CrossPointSettings::syntheticTocFallback,
                           "syntheticTocFallback", StrId::STR_CAT_READER)
           .withSubcategory(StrId::STR_MENU_READER_TWEAKS));
  emit(SettingInfo::Toggle(StrId::STR_BIONIC_READING, &CrossPointSettings::bionicReading, "bionicReading",
                           StrId::STR_CAT_READER)
           .withSubmenu(StrId::STR_READING_AIDS));
  emit(SettingInfo::Toggle(StrId::STR_GUIDE_DOTS, &CrossPointSettings::guideDots, "guideDots", StrId::STR_CAT_READER)
           .withSubmenu(StrId::STR_READING_AIDS));
  emit(SettingInfo::Toggle(StrId::STR_INLINE_FOOTNOTE_PREVIEWS, &CrossPointSettings::inlineFootnotePreviews,
                           "inlineFootnotePreviews", StrId::STR_CAT_READER));

  // --- Controls ---
  // --- Button Actions (short / double / long press per logical button) ---
  // All entries share the same ordered action-label list; the submenu groups them
  // behind a single placeholder row in the device UI.
  // Shared action options (everything except the first "default" entry).
  // Back button: short=exit reader, double=ignore, long=go home
  emit(SettingInfo::Enum(StrId::STR_BTN_SHORT_PRESS, &CrossPointSettings::btnShortBack,
                         {StrId::STR_BTN_DEF_EXIT_READER}, "btnShortBack", StrId::STR_CAT_CONTROLS)
           .withSubcategory(StrId::STR_MENU_BTN_ACTIONS)
           .withSubmenu(StrId::STR_BTN_BACK));
  emit(SettingInfo::Enum(StrId::STR_BTN_DOUBLE_PRESS, &CrossPointSettings::btnDoubleBack,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_IGNORE), "btnDoubleBack", StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_BACK)
           .withSelectorActivity());
  emit(SettingInfo::Enum(StrId::STR_BTN_LONG_PRESS, &CrossPointSettings::btnLongBack,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_GO_HOME), "btnLongBack", StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_BACK)
           .withSelectorActivity());
  // Confirm button: short=reader menu, double=ignore, long=KOReader sync
  emit(SettingInfo::Enum(StrId::STR_BTN_SHORT_PRESS, &CrossPointSettings::btnShortConfirm,
                         {StrId::STR_BTN_DEF_READER_MENU}, "btnShortConfirm", StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_CONFIRM));
  emit(SettingInfo::Enum(StrId::STR_BTN_DOUBLE_PRESS, &CrossPointSettings::btnDoubleConfirm,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_IGNORE), "btnDoubleConfirm", StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_CONFIRM)
           .withSelectorActivity());
  emit(SettingInfo::Enum(StrId::STR_BTN_LONG_PRESS, &CrossPointSettings::btnLongConfirm,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_KOREADER_SYNC), "btnLongConfirm",
                         StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_CONFIRM)
           .withSelectorActivity());
  // Left button: short=previous page, double=ignore, long=chapter back
  emit(SettingInfo::Enum(StrId::STR_BTN_SHORT_PRESS, &CrossPointSettings::btnShortLeft,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_PREV_PAGE), "btnShortLeft", StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_LEFT)
           .withSelectorActivity());
  emit(SettingInfo::Enum(StrId::STR_BTN_DOUBLE_PRESS, &CrossPointSettings::btnDoubleLeft,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_IGNORE), "btnDoubleLeft", StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_LEFT)
           .withSelectorActivity());
  emit(SettingInfo::Enum(StrId::STR_BTN_LONG_PRESS, &CrossPointSettings::btnLongLeft,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_CHAPTER_BACK), "btnLongLeft", StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_LEFT)
           .withSelectorActivity());
  // Right button: short=next page, double=ignore, long=chapter forward
  emit(SettingInfo::Enum(StrId::STR_BTN_SHORT_PRESS, &CrossPointSettings::btnShortRight,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_NEXT_PAGE), "btnShortRight", StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_RIGHT)
           .withSelectorActivity());
  emit(SettingInfo::Enum(StrId::STR_BTN_DOUBLE_PRESS, &CrossPointSettings::btnDoubleRight,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_IGNORE), "btnDoubleRight", StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_RIGHT)
           .withSelectorActivity());
  emit(SettingInfo::Enum(StrId::STR_BTN_LONG_PRESS, &CrossPointSettings::btnLongRight,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_CHAPTER_FORWARD), "btnLongRight",
                         StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_RIGHT)
           .withSelectorActivity());
  // Page Back button: short=previous page, double=ignore, long=chapter back
  emit(SettingInfo::Enum(StrId::STR_BTN_SHORT_PRESS, &CrossPointSettings::btnShortPageBack,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_PREV_PAGE), "btnShortPageBack",
                         StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_UP)
           .withSelectorActivity());
  emit(SettingInfo::Enum(StrId::STR_BTN_DOUBLE_PRESS, &CrossPointSettings::btnDoublePageBack,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_IGNORE), "btnDoublePageBack", StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_UP)
           .withSelectorActivity());
  emit(SettingInfo::Enum(StrId::STR_BTN_LONG_PRESS, &CrossPointSettings::btnLongPageBack,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_CHAPTER_BACK), "btnLongPageBack",
                         StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_UP)
           .withSelectorActivity());
  // Page Forward button: short=next page, double=ignore, long=chapter forward
  emit(SettingInfo::Enum(StrId::STR_BTN_SHORT_PRESS, &CrossPointSettings::btnShortPageForward,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_NEXT_PAGE), "btnShortPageForward",
                         StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_DOWN)
           .withSelectorActivity());
  emit(SettingInfo::Enum(StrId::STR_BTN_DOUBLE_PRESS, &CrossPointSettings::btnDoublePageForward,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_IGNORE), "btnDoublePageForward",
                         StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_DOWN)
           .withSelectorActivity());
  emit(SettingInfo::Enum(StrId::STR_BTN_LONG_PRESS, &CrossPointSettings::btnLongPageForward,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_CHAPTER_FORWARD), "btnLongPageForward",
                         StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_DOWN)
           .withSelectorActivity());
  // Power button: short=ignore, double=ignore, long=sleep (via hold timer, not event system)
  emit(SettingInfo::Enum(StrId::STR_BTN_SHORT_PRESS, &CrossPointSettings::btnShortPower,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_IGNORE), "btnShortPower", StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_POWER)
           .withSelectorActivity());
  emit(SettingInfo::Enum(StrId::STR_BTN_DOUBLE_PRESS, &CrossPointSettings::btnDoublePower,
                         makeBtnActionOptions(StrId::STR_BTN_DEF_IGNORE), "btnDoublePower", StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_POWER)
           .withSelectorActivity());
  emit(SettingInfo::Enum(StrId::STR_BTN_LONG_PRESS, &CrossPointSettings::btnLongPower, {StrId::STR_BTN_DEF_SLEEP},
                         "btnLongPower", StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_BTN_POWER));
  // --- Gesture actions (reader only) ---
  // Generated from TouchGestures::BINDINGS rather than written out row by row:
  // eighteen near-identical push_backs is exactly the kind of ladder that drifts
  // out of step with the table the input layer reads. Every row offers the same
  // action list as a button, and every one defaults to Built-in — which for a
  // gesture means "leave this contact to the reader" — and the label on that
  // entry names what the reader will then do with it, the way each button row
  // names its own default, rather than a bare "Built-in" that answers nothing.
  // Built then filtered out again on a board with no digitiser: twenty SettingInfo
  // rows, each with its own ~66-byte option vector, constructed and freed on every
  // walk of the settings -- which JsonSettingsIO makes on every settings SAVE and LOAD,
  // not just when the settings screen opens. Gated rather than left to the board
  // filter in forEachSetting(), which only runs once a row has been built.
#if CP_TOUCH_UI
  // Touch reading controls (touch boards only — gated on the capability, not on
  // a board name, so X4 Pro and T5S3 both get them).
  emit(SettingInfo::Enum(StrId::STR_TOUCH_UI_CONTROLS, &CrossPointSettings::touchUiControls,
                         {StrId::STR_TOUCH_UI_OFF, StrId::STR_TOUCH_UI_ON}, "touchUiControls", StrId::STR_CAT_CONTROLS)
           .withSubcategory(StrId::STR_TOUCH_UI_CONTROLS)
           .requiring(SettingRequires::TouchPanel));
  emit(SettingInfo::Enum(StrId::STR_TOUCH_LIST_ACTIVATION, &CrossPointSettings::touchListActivation,
                         {StrId::STR_TOUCH_LIST_SELECT_THEN_ACTIVATE, StrId::STR_TOUCH_LIST_ACTIVATE_IMMEDIATELY},
                         "touchListAct", StrId::STR_CAT_CONTROLS)
           .withSubcategory(StrId::STR_TOUCH_UI_CONTROLS)
           .requiring(SettingRequires::TouchPanel));
  emit(SettingInfo::Enum(StrId::STR_TOUCH_READER_CONTROLS, &CrossPointSettings::touchReaderControls,
                         {StrId::STR_TOUCH_READER_OFF, StrId::STR_TOUCH_READER_TAP, StrId::STR_TOUCH_READER_SWIPE,
                          StrId::STR_TOUCH_READER_INVERTED},
                         "touchReaderCtl", StrId::STR_CAT_CONTROLS)
           .requiring(SettingRequires::TouchPanel));
  for (const auto& binding : TouchGestures::BINDINGS) {
    // cppcheck-suppress useStlAlgorithm ; std::transform would have to carry this
    // whole chained builder in a lambda and append through a back_inserter, which
    // is longer and reads worse than the loop.
    emit(SettingInfo::Enum(binding.label, binding.field,
                           makeBtnActionOptions(TouchGestures::builtinLabelFor(binding.gesture)), binding.key,
                           StrId::STR_CAT_CONTROLS)
             .withSubmenu(StrId::STR_MENU_GESTURE_ACTIONS)
             .withSubcategory(binding.group)
             .withSelectorActivity()
             .requiring(binding.needsMultiTouch ? SettingRequires::MultiTouchPanel : SettingRequires::TouchPanel));
  }
#endif  // CP_TOUCH_UI
  // Tilt page turn (X3-only)
  emit(SettingInfo::Toggle(StrId::STR_TILT_PAGE_TURN, &CrossPointSettings::tiltPageTurn, "tiltPageTurn",
                           StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_TILT_PAGE_TURN)
           .requiring(SettingRequires::TiltSensor));
  emit(SettingInfo::Enum(StrId::STR_DIR_RIGHT, &CrossPointSettings::tiltPositiveAction,
                         {StrId::STR_NONE_OPT, StrId::STR_NEXT_PAGE, StrId::STR_PREV_PAGE}, "tiltPositiveAction",
                         StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_TILT_PAGE_TURN)
           .requiring(SettingRequires::TiltSensor));
  emit(SettingInfo::Enum(StrId::STR_DIR_LEFT, &CrossPointSettings::tiltNegativeAction,
                         {StrId::STR_NONE_OPT, StrId::STR_NEXT_PAGE, StrId::STR_PREV_PAGE}, "tiltNegativeAction",
                         StrId::STR_CAT_CONTROLS)
           .withSubmenu(StrId::STR_TILT_PAGE_TURN)
           .requiring(SettingRequires::TiltSensor));
  // --- System ---
  emit(SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles, "showHiddenFiles",
                           StrId::STR_CAT_SYSTEM)
           .withSubmenu(StrId::STR_SHOW_FILES));
  emit(SettingInfo::Toggle(StrId::STR_SHOW_FILE_EXTENSIONS, &CrossPointSettings::showFileExtensions,
                           "showFileExtensions", StrId::STR_CAT_SYSTEM)
           .withSubmenu(StrId::STR_SHOW_FILES));
  emit(SettingInfo::Enum(StrId::STR_BROWSER_VIEW, &CrossPointSettings::fileBrowserView,
                         {StrId::STR_VIEW_FILENAMES, StrId::STR_VIEW_DETAILS, StrId::STR_VIEW_COVERS},
                         "fileBrowserView", StrId::STR_CAT_SYSTEM)
           .withSubmenu(StrId::STR_SHOW_FILES));
  emit(SettingInfo::Toggle(StrId::STR_INCLUDE_BETA_UPDATES, &CrossPointSettings::includeBetaUpdates, "includeRcUpdates",
                           StrId::STR_CAT_SYSTEM));
  emit(SettingInfo::Toggle(StrId::STR_SKIP_HTTPS_VALIDATION, &CrossPointSettings::skipHttpsValidation,
                           "skipHttpsValidation", StrId::STR_CAT_SYSTEM));
  // OPDS download settings are edited from the OPDS server list. Keep them
  // category-less so they persist and remain available to the web API without
  // cluttering the device settings screen.
  emit(SettingInfo::String(StrId::STR_OPDS_DOWNLOAD_FOLDER, SETTINGS.opdsDownloadFolder,
                           sizeof(SETTINGS.opdsDownloadFolder), "opdsDownloadFolder"));
  emit(SettingInfo::Enum(StrId::STR_OPDS_FILENAME_FORMAT, &CrossPointSettings::opdsFilenameFormat,
                         {StrId::STR_FMT_AUTHOR_TITLE, StrId::STR_FMT_TITLE_AUTHOR, StrId::STR_FMT_TITLE},
                         "opdsFilenameFormat"));
  // Will be dealt with separately, so these receive none of the main categories and
  // are visible in the web UI but not the device UI.
  emit(SettingInfo::Toggle(StrId::STR_USE_CLOCK, &CrossPointSettings::useClock, "useClock", StrId::STR_CLOCK));
  emit(SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat12h, {StrId::STR_24H, StrId::STR_12H},
                         "clockFormat12h", StrId::STR_CLOCK));
  // Every zone the table defines, via the shared factory -- not a list maintained here. The
  // hand-written copy this replaces had stopped at 16 of 19, and because JsonSettingsIO bounds
  // a loaded ENUM on THIS row's option count, the three it omitted reset to the default on
  // every boot after being chosen. See TimezoneOptions.h.
  emit(TimezoneOptions::make(StrId::STR_CLOCK));
  emit(SettingInfo::String(StrId::STR_NTP_SERVER, SETTINGS.ntpServer, sizeof(SETTINGS.ntpServer), "ntpServer",
                           StrId::STR_CLOCK));
  // Weather
  emit(SettingInfo::Toggle(StrId::STR_USE_WEATHER, &CrossPointSettings::useWeather, "useWeather", StrId::STR_WEATHER));

  // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
  emit(SettingInfo::DynamicString(
      StrId::STR_SYNC_SERVER_URL, static_cast<SettingInfo::StringGetterFn>(getKoReaderServerUrl),
      [](void*, const std::string& v) {
        KOREADER_STORE.setServerUrl(v);
        KOREADER_STORE.saveToFile();
      },
      "koServerUrl", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicString(
      StrId::STR_KOREADER_USERNAME, static_cast<SettingInfo::StringGetterFn>(getKoReaderUsername),
      [](void*, const std::string& v) {
        KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
        KOREADER_STORE.saveToFile();
      },
      "koUsername", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicString(
           StrId::STR_KOREADER_PASSWORD, static_cast<SettingInfo::StringGetterFn>(getKoReaderPassword),
           [](void*, const std::string& v) {
             KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
             KOREADER_STORE.saveToFile();
           },
           "koPassword", StrId::STR_KOREADER_SYNC)
           .withObfuscated());
  emit(SettingInfo::DynamicEnum(
      StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY}, getKoReaderMatchMethod,
      [](void*, uint8_t v) {
        KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
        KOREADER_STORE.saveToFile();
      },
      "koMatchMethod", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicEnum(
      StrId::STR_KO_SYNC_CONFLICT, {StrId::STR_KO_ASK_EVERY_TIME, StrId::STR_KO_SMART_SYNC},
      [](const void*) -> uint8_t { return static_cast<uint8_t>(KOREADER_STORE.getSyncBehavior()); },
      [](void*, uint8_t v) {
        KOREADER_STORE.setSyncBehavior(static_cast<KOReaderSyncBehavior>(v));
        KOREADER_STORE.saveToFile();
      },
      "koSyncBehavior", StrId::STR_KOREADER_SYNC));
  emit([]() {
    SettingInfo s;
    s.nameId = StrId::STR_SEND_METADATA;
    s.type = SettingType::TOGGLE;
    s.key = "koSendMetadata";
    s.category = StrId::STR_KOREADER_SYNC;
    s.valueGetter = [](const void*) -> uint8_t { return KOREADER_STORE.getSendMetadata() ? 1u : 0u; };
    s.valueSetter = [](void*, uint8_t v) {
      KOREADER_STORE.setSendMetadata(v != 0);
      KOREADER_STORE.saveToFile();
    };
    return s;
  }());

#if CROSSPOINT_KOREADER_AUTOSYNC
  emit(SettingInfo::DynamicToggle(
           StrId::STR_KO_AUTO_ON_WAKE, [](const void*) -> uint8_t { return KOREADER_STORE.getSyncOnWake() ? 1u : 0u; },
           [](void*, uint8_t v) {
             KOREADER_STORE.setSyncOnWake(v != 0);
             KOREADER_STORE.saveToFile();
           },
           "koSyncOnWake", StrId::STR_KOREADER_SYNC)
           .withSubmenu(StrId::STR_MENU_KOSYNC_AUTO));
  emit(SettingInfo::DynamicValue(
           StrId::STR_KO_AUTO_WHILE_READING, {0, KOReaderCredentialStore::PUSH_INTERVAL_MAX_PAGES, 1},
           [](const void*) -> uint8_t { return static_cast<uint8_t>(KOREADER_STORE.getPushIntervalPages()); },
           [](void*, uint8_t v) {
             KOREADER_STORE.setPushIntervalPages(v);
             KOREADER_STORE.saveToFile();
           },
           "koPushIntervalPages", StrId::STR_KOREADER_SYNC)
           .withSubmenu(StrId::STR_MENU_KOSYNC_AUTO));
  emit(SettingInfo::DynamicToggle(
           StrId::STR_KO_AUTO_ON_SLEEP,
           [](const void*) -> uint8_t { return KOREADER_STORE.getSyncOnSleep() ? 1u : 0u; },
           [](void*, uint8_t v) {
             KOREADER_STORE.setSyncOnSleep(v != 0);
             KOREADER_STORE.saveToFile();
           },
           "koSyncOnSleep", StrId::STR_KOREADER_SYNC)
           .withSubmenu(StrId::STR_MENU_KOSYNC_AUTO));
#endif  // CROSSPOINT_KOREADER_AUTOSYNC
  emit(SettingInfo::Toggle(StrId::STR_KO_AUTO_ON_CLOSE, &CrossPointSettings::koSyncOnBookClose, "koSyncOnBookClose",
                           StrId::STR_KOREADER_SYNC)
           .withSubmenu(StrId::STR_MENU_KOSYNC_AUTO));
  emit(SettingInfo::Value(StrId::STR_KO_MIN_SESSION_PAGES, &CrossPointSettings::koSyncMinSessionPages, {0, 60, 1},
                          "koSyncMinSessionPages", StrId::STR_KOREADER_SYNC)
           .withSubmenu(StrId::STR_MENU_KOSYNC_AUTO));
#if CROSSPOINT_KOREADER_AUTOSYNC
  emit(SettingInfo::DynamicToggle(
           StrId::STR_KO_SHOW_SYNC_INDICATOR,
           [](const void*) -> uint8_t { return KOREADER_STORE.getShowSyncIndicator() ? 1u : 0u; },
           [](void*, uint8_t v) {
             KOREADER_STORE.setShowSyncIndicator(v != 0);
             KOREADER_STORE.saveToFile();
           },
           "koShowSyncIndicator", StrId::STR_KOREADER_SYNC)
           .withSubmenu(StrId::STR_MENU_KOSYNC_AUTO));
#endif  // CROSSPOINT_KOREADER_AUTOSYNC

  // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
  emit(SettingInfo::Enum(StrId::STR_UPPER_PROGRESS_BAR, &CrossPointSettings::statusBarUpperProgressBar,
                         {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarUpperProgressBar",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_UPPER_PROGRESS_BAR_THICKNESS,
                         &CrossPointSettings::statusBarUpperProgressBarThickness,
                         {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                         "statusBarUpperProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_STATUS_ITEMS_POSITION, &CrossPointSettings::statusBarItemsPosition,
                         {StrId::STR_TOP, StrId::STR_BOTTOM}, "statusBarItemsPosition",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Toggle(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount,
                           "statusBarChapterPageCount", StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Toggle(StrId::STR_PRINTED_PAGE_NUMBER, &CrossPointSettings::statusBarPrintedPage,
                           "statusBarPrintedPage", StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarBookProgressPercentage,
                           "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                         {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarTitle",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Toggle(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery, "statusBarBattery",
                           StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Toggle(StrId::STR_CLOCK, &CrossPointSettings::statusBarClock, "statusBarClock",
                           StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_CLOCK_POSITION, &CrossPointSettings::statusBarClockPosition,
                         {StrId::STR_ALIGN_LEFT, StrId::STR_ALIGN_RIGHT}, "statusBarClockPosition",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_LOWER_PROGRESS_BAR, &CrossPointSettings::statusBarLowerProgressBar,
                         {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarLowerProgressBar",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_LOWER_PROGRESS_BAR_THICKNESS,
                         &CrossPointSettings::statusBarLowerProgressBarThickness,
                         {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                         "statusBarLowerProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR));
}

// Answers each capability ONCE, from the HAL or the active board profile, so the rest of the
// codebase never has to ask "which board is this" to decide whether a setting is meaningful.
inline bool boardHas(const SettingRequires capability) {
  switch (capability) {
    case SettingRequires::Nothing:
      return true;
    case SettingRequires::TouchPanel:
      return gpio.hasTouch();
    case SettingRequires::TiltSensor:
      return BoardConfig::ACTIVE.sensors.imuType != BoardConfig::ImuType::None;
    case SettingRequires::SelectableGrayscaleLut:
      // SSD1677 develops grayscale from a factory waveform, so there is no
      // LUT to swap and the fast/OEM trade-off does not exist there.
      return BoardConfig::ACTIVE.displayController != BoardConfig::DisplayController::SSD1677;
    case SettingRequires::ReadingLight:
      // Asked of the HAL rather than the board profile: the EEGO A4's I2C
      // light is only present after begin() gets an ACK, so the profile alone
      // would over-report.
      return Frontlight.present();
    case SettingRequires::WarmLight:
      return Frontlight.present() && Frontlight.hasColorTemperature();
    case SettingRequires::MultiTouchPanel:
      return gpio.hasTouch() && gpio.supportsMultiTouch();
    case SettingRequires::SunlightFadingPanel:
      return HalCapabilities::panelFadesInSunlight();
  }
  return true;
}

// Whether settings.json keeps a row: what the board's profile says it has, not what answered at
// this boot. boardHas() asks the touch controller's probe, and a probe that missed one boot hid the
// touch rows from that boot's load and dropped them from its next save, so the boot after, with the
// controller answering again, started every gesture and touch control from its default. The other
// capabilities are answered from the profile already (the light by Frontlight.present() on every
// board this firmware builds for), so they keep what boardHas() says.
//
// Based on crosspoint-reader PR #4078 (Tuan Q. Nguyen), which keeps the tilt row in the persisted
// list when the IMU probe fails; tilt is answered from the profile here, the touch rows were not.
inline bool boardKeeps(const SettingRequires capability) {
  switch (capability) {
    case SettingRequires::TouchPanel:
      return HalCapabilities::hasTouch();
    case SettingRequires::MultiTouchPanel:
      return HalCapabilities::hasMultiTouch();
    case SettingRequires::Nothing:
    case SettingRequires::TiltSensor:
    case SettingRequires::SelectableGrayscaleLut:
    case SettingRequires::ReadingLight:
    case SettingRequires::WarmLight:
    case SettingRequires::SunlightFadingPanel:
      return boardHas(capability);
  }
  return true;
}

// The walk forEachSetting() and forEachPersistedSetting() share, filtered by `offers`.
template <bool (*offers)(SettingRequires), typename Visit>
void visitRows(Visit&& visit) {
  using Visitor = std::remove_reference_t<Visit>;
  const RowSink sink{const_cast<void*>(static_cast<const void*>(&visit)), [](void* visitor, SettingInfo& row) {
                       if (offers(row.requiredCapability)) (*static_cast<Visitor*>(visitor))(row);
                     }};
  buildSettings(sink);
}

}  // namespace SettingsListDetail

// Calls `visit(SettingInfo&)` for every setting this board offers, in list order. Each row is
// built, visited and destroyed before the next one is built, so a walk costs one row's heap,
// never the list's; a visitor that keeps a row moves it out.
//
// This replaced a getSettingsList() that returned every row in one std::vector: ~115 rows of
// 100 bytes, an 11.5 KB contiguous block. Settings saves and loads, the settings screens and
// the web settings API each built it, and under -fno-exceptions a refused block is an abort:
// an X3 aborted in that reserve on the settings screen with 26 KB of fragmented heap free.
template <typename Visit>
void forEachSetting(Visit&& visit) {
  SettingsListDetail::visitRows<&SettingsListDetail::boardHas>(visit);
}

// forEachSetting() for settings.json: also the rows of hardware the board has but that did not
// answer at this boot (SettingsListDetail::boardKeeps), so a load and a save never forget them.
template <typename Visit>
void forEachPersistedSetting(Visit&& visit) {
  SettingsListDetail::visitRows<&SettingsListDetail::boardKeeps>(visit);
}
