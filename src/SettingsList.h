#pragma once

#include <BoardConfig.h>
#include <HalClock.h>
#include <HalFrontlight.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <SdCardFontRegistry.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "HomeButtonSettings.h"
#include "KOReaderCredentialStore.h"
#include "ReaderFontSizes.h"
#include "ReaderLineSpacing.h"
#include "activities/settings/SettingsActivity.h"
#include "components/UITheme.h"
#include "util/DictionaryRegistry.h"
#include "util/SlideSwitch.h"

// Build the font family setting dynamically. When registry is non-null, SD card fonts
// are appended after the built-in fonts. Otherwise only built-in fonts are listed.
inline SettingInfo buildFontFamilySetting(const SdCardFontRegistry* registry) {
  // The render code checks enumStringValues first, then enumValues, so with SD
  // fonts present every option (built-in first) is listed as a string.
  std::vector<std::string> allStringValues;
  if (registry && registry->getFamilyCount() > 0) {
    const auto& families = registry->getFamilies();
    allStringValues.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + families.size());
    allStringValues.push_back(I18N.get(StrId::STR_NOTO_SANS));
    std::transform(families.begin(), families.end(), std::back_inserter(allStringValues),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  SettingInfo s;
  s.nameId = StrId::STR_FONT_FAMILY;
  s.type = SettingType::ENUM;
  s.enumValues = {StrId::STR_NOTO_SANS};
  s.enumStringValues = std::move(allStringValues);
  s.key = "fontFamily";
  s.category = StrId::STR_CAT_READER;
  s.inTextSettings = true;  // matches the static font-family entry it replaces

  // The lambdas read the registry instead of copying the names: it is only
  // rescanned while no list built from it is in use, so the indexes match.
  s.valueGetter = [registry]() -> uint8_t {
    // If an SD card font is selected, find its index
    if (registry && SETTINGS.sdFontFamilyName[0] != '\0') {
      const auto& families = registry->getFamilies();
      for (size_t i = 0; i < families.size(); i++) {
        if (families[i].name == SETTINGS.sdFontFamilyName) {
          return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i);
        }
      }
      // SD font name not found in registry — fall through to built-in
    }
    return SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
  };

  s.valueSetter = [registry](uint8_t v) {
    if (v < CrossPointSettings::BUILTIN_FONT_COUNT) {
      SETTINGS.fontFamily = v;
      SETTINGS.sdFontFamilyName[0] = '\0';
    } else if (registry) {
      const size_t sdIdx = v - CrossPointSettings::BUILTIN_FONT_COUNT;
      const auto& families = registry->getFamilies();
      if (sdIdx < families.size()) {
        strncpy(SETTINGS.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
        SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      }
    }
  };

  return s;
}

// Build the font size setting dynamically: the options are the point sizes the
// active family actually ships, so an SD family built at 10/12/14 offers three
// sizes and a family built at 8..18 offers six. The selected point size persists
// in SETTINGS.fontPointSize (saved/loaded manually in CrossPointSettings::
// toJson/fromJson — the generic loop skips dynamic entries), while the ENUM
// contract shared with the web UI stays index-based.
inline SettingInfo buildFontSizeSetting(const SdCardFontRegistry* registry) {
  // Captured by copy so the indexes keep matching the option labels built here,
  // even if the family changes later.
  const std::vector<uint8_t> sizes = readerFontPointSizes(registry, SETTINGS.sdFontFamilyName);

  // "pt" is deliberately not translated — see the matching note in
  // TextSettingsActivity::rebuildSizeList().
  std::vector<std::string> labels;
  labels.reserve(sizes.size());
  for (const uint8_t pt : sizes) {
    labels.push_back(std::to_string(pt) + " pt");
  }

  SettingInfo s;
  s.nameId = StrId::STR_FONT_SIZE;
  s.type = SettingType::ENUM;
  s.enumStringValues = std::move(labels);
  s.key = "fontSize";
  s.category = StrId::STR_CAT_READER;
  s.inTextSettings = true;  // matches the static font-size entry it replaces

  s.valueGetter = [sizes]() -> uint8_t {
    const uint8_t pt = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
    for (int i = 0; i < static_cast<int>(sizes.size()); i++) {
      if (sizes[i] == pt) return static_cast<uint8_t>(i);
    }
    return 0;
  };

  s.valueSetter = [sizes](uint8_t v) {
    if (v < sizes.size()) SETTINGS.fontPointSize = sizes[v];
  };

  return s;
}

inline constexpr char DICTIONARY_SETTING_KEY[] = "dictionaryName";

// Build the dictionary selection setting dynamically from the folders discovered
// under /dictionaries. "None" plus one option per dictionary; the selected folder
// name persists in SETTINGS.dictionaryName (saved/loaded manually in
// CrossPointSettings::toJson/fromJson — the generic loop skips dynamic entries).
inline SettingInfo buildDictionarySetting(const std::vector<DictionaryEntry>& dictionaries) {
  std::vector<std::string> folderNames;
  folderNames.reserve(dictionaries.size());
  std::transform(dictionaries.begin(), dictionaries.end(), std::back_inserter(folderNames),
                 [](const DictionaryEntry& d) { return d.name; });

  SettingInfo s;
  s.nameId = StrId::STR_DICTIONARY;
  s.key = DICTIONARY_SETTING_KEY;  // web settings API; persisted by name, not by the generic loop
  s.type = SettingType::ENUM;
  s.enumStringValues.reserve(folderNames.size() + 1);
  s.enumStringValues.push_back(I18N.get(StrId::STR_NONE_OPT));
  s.enumStringValues.insert(s.enumStringValues.end(), folderNames.begin(), folderNames.end());
  s.category = StrId::STR_CAT_READER;

  s.valueGetter = [folderNames]() -> uint8_t {
    for (size_t i = 0; i < folderNames.size(); i++) {
      // Compare within the settings field capacity: an over-long folder name is
      // stored truncated, and must still match its list entry.
      if (strncmp(folderNames[i].c_str(), SETTINGS.dictionaryName, sizeof(SETTINGS.dictionaryName) - 1) == 0) {
        return static_cast<uint8_t>(i + 1);
      }
    }
    return 0;  // "None", also when the stored folder no longer exists
  };

  s.valueSetter = [folderNames](uint8_t v) {
    if (v == 0 || v > folderNames.size()) {
      SETTINGS.dictionaryName[0] = '\0';
      return;
    }
    strncpy(SETTINGS.dictionaryName, folderNames[v - 1].c_str(), sizeof(SETTINGS.dictionaryName) - 1);
    SETTINGS.dictionaryName[sizeof(SETTINGS.dictionaryName) - 1] = '\0';
  };

  return s;
}

inline std::vector<StrId> buildLongPressMenuValues() {
  std::vector<StrId> values = {StrId::STR_KOSYNC, StrId::STR_DISABLED, StrId::STR_BOOKMARK_OPTION,
                               StrId::STR_DICTIONARY};
  values.reserve(CrossPointSettings::LONG_PRESS_MENU_FUNCTION_COUNT);
  if (BoardConfig::hasHomeKey()) values.push_back(StrId::STR_READER_MENU);
  values.push_back(StrId::STR_SAVE_CLIPPING);
  values.push_back(StrId::STR_FOOTNOTES);
  return values;
}

inline uint8_t longPressMenuDisplayValue() {
  const uint8_t raw = SETTINGS.longPressMenuFunction;
  if (raw <= CrossPointSettings::LP_MENU_DICTIONARY) return raw;
  if (raw == CrossPointSettings::LP_MENU_READER_MENU) {
    return BoardConfig::hasHomeKey() ? CrossPointSettings::LP_MENU_READER_MENU : CrossPointSettings::LP_MENU_DISABLED;
  }
  if (raw < CrossPointSettings::LONG_PRESS_MENU_FUNCTION_COUNT) return BoardConfig::hasHomeKey() ? raw : raw - 1;
  return CrossPointSettings::LP_MENU_DISABLED;
}

inline void setLongPressMenuFromDisplayValue(const uint8_t displayValue) {
  if (displayValue <= CrossPointSettings::LP_MENU_DICTIONARY) {
    SETTINGS.longPressMenuFunction = displayValue;
  } else {
    SETTINGS.longPressMenuFunction = BoardConfig::hasHomeKey() ? displayValue : displayValue + 1;
  }
}

inline SettingInfo buildLongPressMenuSetting() {
  return SettingInfo::DynamicEnum(StrId::STR_LONG_PRESS_MENU, buildLongPressMenuValues(), longPressMenuDisplayValue,
                                  setLongPressMenuFromDisplayValue, "longPressMenuFunction", StrId::STR_CAT_CONTROLS);
}

struct ShortPowerButtonOption {
  StrId label;
  uint8_t value;
};

// Menu order differs from the persisted enum order, so map by value.
inline std::vector<ShortPowerButtonOption> shortPowerButtonOptions() {
  std::vector<ShortPowerButtonOption> options = {{StrId::STR_IGNORE, CrossPointSettings::IGNORE},
                                                 {StrId::STR_SLEEP, CrossPointSettings::SLEEP},
                                                 {StrId::STR_PAGE_TURN, CrossPointSettings::PAGE_TURN},
                                                 {StrId::STR_FORCE_REFRESH, CrossPointSettings::FORCE_REFRESH},
                                                 {StrId::STR_FOOTNOTES, CrossPointSettings::FOOTNOTES}};
  options.reserve(CrossPointSettings::SHORT_PWRBTN_COUNT);
  if (BoardConfig::hasTouch()) {
    options.push_back({StrId::STR_CONFIRM, CrossPointSettings::PWR_CONFIRM});
  } else {
    options.push_back({StrId::STR_SAVE_CLIPPING, CrossPointSettings::CREATE_CLIPPING});
  }
  // Touch boards without a home key have no button for these shortcuts.
  if (BoardConfig::hasTouch() && !BoardConfig::hasHomeKey()) {
    options.push_back({StrId::STR_BOOKMARK_OPTION, CrossPointSettings::PWR_BOOKMARK});
    options.push_back({StrId::STR_KOSYNC, CrossPointSettings::PWR_SYNC});
    options.push_back({StrId::STR_READER_MENU, CrossPointSettings::PWR_READER_MENU});
  }
  return options;
}

inline std::vector<StrId> buildShortPowerButtonValues() {
  std::vector<StrId> values;
  for (const auto& option : shortPowerButtonOptions()) values.push_back(option.label);
  return values;
}

inline uint8_t shortPowerButtonDisplayValue() {
  const auto options = shortPowerButtonOptions();
  for (size_t i = 0; i < options.size(); i++) {
    if (options[i].value == SETTINGS.shortPwrBtn) return static_cast<uint8_t>(i);
  }
  return CrossPointSettings::IGNORE;
}

inline void setShortPowerButtonFromDisplayValue(const uint8_t displayValue) {
  const auto options = shortPowerButtonOptions();
  SETTINGS.shortPwrBtn = displayValue < options.size() ? options[displayValue].value : CrossPointSettings::IGNORE;
}

inline SettingInfo buildShortPowerButtonSetting() {
  return SettingInfo::DynamicEnum(StrId::STR_SHORT_PWR_BTN, buildShortPowerButtonValues(), shortPowerButtonDisplayValue,
                                  setShortPowerButtonFromDisplayValue, "shortPwrBtn", StrId::STR_CAT_CONTROLS);
}

inline std::vector<StrId> homeThemeValues() {
  static constexpr StrId VALUES[] = {StrId::STR_THEME_CLASSIC, StrId::STR_THEME_LYRA, StrId::STR_THEME_LYRA_EXTENDED,
                                     StrId::STR_THEME_ROUNDEDRAFF, StrId::STR_THEME_COVER_GRID};
  const size_t count = UITheme::supportsCoverGrid() ? std::size(VALUES) : std::size(VALUES) - 1;
  return {VALUES, VALUES + count};
}

using SettingVisitor = void (*)(void* context, SettingInfo& info);

// Visits synchronously in list order. Borrowed references are valid only during
// the callback; move from the descriptor to retain ownership. Its board conditions
// must be fixed before settings load, or load and save would see different keys.
void forEachBaseSetting(SettingVisitor visit, void* context);

template <typename Fn>
void forEachBaseSetting(Fn& fn) {
  forEachBaseSetting([](void* context, SettingInfo& info) { (*static_cast<Fn*>(context))(info); }, &fn);
}

// Number of entries forEachBaseSetting() visits.
size_t countBaseSettings();

// True if this board hides the setting at runtime. UI-only: the probes have not
// run at load time, so the base list keeps these keys persisted.
inline bool isHiddenOnDevice(const SettingInfo& s) {
  switch (s.nameId) {
    // Frontlit and lightless variants of a board share one binary (EEGO A4);
    // presence is the I2C probe result from Frontlight.begin(). The slide
    // switch's position owns its bound function (the light at boot too).
    case StrId::STR_RESTORE_LIGHT_ON_WAKE:
      return !Frontlight.present() || slideSwitchOwnsLight();
    case StrId::STR_NIGHT_MODE:
      return slideSwitchOwns(CrossPointSettings::SLIDE_SWITCH_NIGHT_MODE);
    case StrId::STR_TOUCH_READER_CONTROLS:
      return slideSwitchOwns(CrossPointSettings::SLIDE_SWITCH_TOUCH_CONTROLS);
    // Tilt page turn needs an IMU with a gyroscope. A failed probe on one boot
    // must not drop the saved value.
    case StrId::STR_TILT_PAGE_TURN:
      return !halTiltSensor.isAvailable();
    // Clock settings need the RTC, as on the device (ClockSettingsActivity).
    case StrId::STR_CLOCK:
    case StrId::STR_CLOCK_FORMAT:
    case StrId::STR_TIMEZONE:
    case StrId::STR_CLOCK_DST:
    case StrId::STR_CLOCK_IN_HEADER:
    case StrId::STR_CLOCK_SYNCED:
      return !halClock.isAvailable();
    // Hidden on touch boards: the Home button actions (X4 Pro) or the short
    // power click (other touch boards) cover these shortcuts.
    case StrId::STR_LONG_PRESS_MENU:
      return BoardConfig::hasTouch();
    default:
      return false;
  }
}

// Replaces the font family and size entries with options built from the fonts
// present; other entries are left unchanged.
inline void buildFontOptions(SettingInfo& s, const SdCardFontRegistry* registry) {
  if (s.nameId == StrId::STR_FONT_FAMILY && registry && registry->getFamilyCount() > 0) {
    s = buildFontFamilySetting(registry);
  } else if (s.nameId == StrId::STR_FONT_SIZE) {
    // Unconditional: even with no SD fonts installed the sizes come from the
    // built-in family rather than a fixed Small/Medium/Large/XL enum.
    s = buildFontSizeSetting(registry);
  }
}

// Visits the settings shown on this device in list order, one at a time, for
// callers that need not hold the whole list in memory. Entries whose key
// wants(key) rejects are skipped before their options are built; wants gets
// nullptr for entries without a key (actions). The visited entry is valid only
// during the callback; move from it to keep it.
template <typename Wants, typename Visit>
void forEachSetting(const SdCardFontRegistry* registry, const std::vector<DictionaryEntry>* dictionaries, Wants wants,
                    Visit visit) {
  bool dictionaryPending = dictionaries && !dictionaries->empty();
  const auto visitDictionary = [&] {
    dictionaryPending = false;
    if (!wants(DICTIONARY_SETTING_KEY)) return;
    SettingInfo s = buildDictionarySetting(*dictionaries);
    visit(s);
  };
  auto each = [&](SettingInfo& s) {
    if (isHiddenOnDevice(s)) return;
    if (s.valuePtr == &CrossPointSettings::vocabularyHistoryEnabled) {
      if (!dictionaryPending) return;
      visitDictionary();
    }
    if (dictionaryPending && s.category == StrId::STR_CAT_CONTROLS) visitDictionary();
    if (!wants(s.key)) return;
    buildFontOptions(s, registry);
    visit(s);
  };
  forEachBaseSetting(each);
  if (dictionaryPending) visitDictionary();
}

// UI callers own the list for runtime filtering and font/dictionary options.
inline std::vector<SettingInfo> getSettingsList(const SdCardFontRegistry* registry = nullptr,
                                                const std::vector<DictionaryEntry>* dictionaries = nullptr) {
  std::vector<SettingInfo> v;
  // Room for every base entry and the dictionary, so the list never reallocates.
  v.reserve(countBaseSettings() + (dictionaries && !dictionaries->empty() ? 1 : 0));
  forEachSetting(
      registry, dictionaries, [](const char*) { return true; }, [&v](SettingInfo& s) { v.push_back(std::move(s)); });
  return v;
}
