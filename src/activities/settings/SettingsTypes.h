#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "CrossPointSettings.h"

// SettingInfo and friends are the data model shared by SettingsActivity
// (on-device UI) and CrossPointWebServer (the JSON settings API). Split into
// its own header, separate from SettingsActivity.h's Activity/rendering
// dependencies, so the model itself -- and SettingsExtension.h, which is
// built on it -- can be included and unit tested without pulling in
// GfxRenderer/MappedInputManager/UiTabListActivity.

// Forward declaration only: an extension's action handler receives the hosting
// activity so it can open a child screen, but this header stays free of
// Activity's rendering dependencies -- the handler takes it by reference and
// never needs the definition here.
class Activity;

// Forward declaration only, for SettingsExtensionCategory::icon below: a
// pointer to an incomplete type keeps this header free of the assets library
// (and of any include path the host tests would have to reproduce).
namespace freeink {
struct Icon;
}

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING };

enum class SettingAction {
  None,
  RemapFrontButtons,
  CustomiseStatusBar,
  KOReaderSync,
  OPDSBrowser,
  Network,
  ClearCache,
  CheckForUpdates,
  SdFirmwareUpdate,
  Language,
  DownloadFonts,
  TextSettings,
  // Dispatches to SettingInfo::actionHandler instead of a case below, so an
  // integration can contribute action rows without adding a value here.
  Extension,
};

struct SettingInfo {
  // Every field carries a default: a row built by a factory that does not set
  // one (ExtensionAction, which has no compiled-in StrId to point nameId at)
  // is still fully initialized. nameId in particular is compared against
  // specific StrIds by the host, so an indeterminate value could silently
  // impersonate a built-in row.
  StrId nameId = StrId::STR_NONE_OPT;
  // ACTION + SettingAction::None (the default below) is the inert combination:
  // it neither mutates a setting nor dispatches anywhere.
  SettingType type = SettingType::ACTION;
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  std::vector<StrId> enumValues;
  std::vector<std::string> enumStringValues;  // runtime alternative to StrId enumValues (for SD card fonts etc.)
  SettingAction action = SettingAction::None;

  struct ValueRange {
    uint8_t min = 0;
    uint8_t max = 0;
    uint8_t step = 0;
  };
  ValueRange valueRange = {};

  const char* key = nullptr;             // JSON API key (nullptr for ACTION types)
  StrId category = StrId::STR_NONE_OPT;  // Category for web UI grouping
  bool obfuscated = false;               // Save/load via base64 obfuscation (passwords)
  bool inTextSettings = false;           // Surfaced in the Text Settings screen; hidden from the flat Reader list

  // Direct char[] string fields (for settings stored in CrossPointSettings)
  size_t stringOffset = 0;
  size_t stringMaxLen = 0;

  // Dynamic accessors (for settings stored outside CrossPointSettings, e.g. KOReaderCredentialStore)
  std::function<uint8_t()> valueGetter;
  std::function<void(uint8_t)> valueSetter;
  std::function<std::string()> stringGetter;
  std::function<void(const std::string&)> stringSetter;

  // Row label override: when non-empty, used in place of I18N.get(nameId).
  // nameId is a compiled-in StrId, so a row contributed at runtime (see
  // SettingsExtension.h) has no StrId of its own to point it at.
  std::string customLabel;

  // Invoked for SettingType::ACTION rows whose action is SettingAction::Extension.
  // Receives the hosting activity, so a handler can open a child screen with
  // startActivityForResult() the same way the built-in ACTION cases do -- an
  // extension row that could only mutate a setting would not be able to
  // contribute anything that navigates.
  std::function<void(Activity&)> actionHandler;

  SettingInfo& withObfuscated() {
    obfuscated = true;
    return *this;
  }

  SettingInfo& withTextSettings() {
    inTextSettings = true;
    return *this;
  }

  SettingInfo& withLabel(const std::string& label) {
    customLabel = label;
    return *this;
  }

  static SettingInfo Toggle(StrId nameId, uint8_t CrossPointSettings::* ptr, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::TOGGLE;
    s.valuePtr = ptr;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Enum(StrId nameId, uint8_t CrossPointSettings::* ptr, std::vector<StrId> values,
                          const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.enumValues = std::move(values);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Action(StrId nameId, SettingAction action) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ACTION;
    s.action = action;
    return s;
  }

  static SettingInfo Value(StrId nameId, uint8_t CrossPointSettings::* ptr, const ValueRange valueRange,
                           const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.valuePtr = ptr;
    s.valueRange = valueRange;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo String(StrId nameId, char* ptr, size_t maxLen, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringOffset = (size_t)ptr - (size_t)&SETTINGS;
    s.stringMaxLen = maxLen;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicEnum(StrId nameId, std::vector<StrId> values, std::function<uint8_t()> getter,
                                 std::function<void(uint8_t)> setter, const char* key = nullptr,
                                 StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.enumValues = std::move(values);
    s.valueGetter = std::move(getter);
    s.valueSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicString(StrId nameId, std::function<std::string()> getter,
                                   std::function<void(const std::string&)> setter, const char* key = nullptr,
                                   StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringGetter = std::move(getter);
    s.stringSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }

  // TOGGLE/VALUE counterpart to DynamicEnum/DynamicString, for a setting
  // whose value lives outside CrossPointSettings.
  static SettingInfo DynamicToggle(StrId nameId, std::function<uint8_t()> getter, std::function<void(uint8_t)> setter,
                                   const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::TOGGLE;
    s.valueGetter = std::move(getter);
    s.valueSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicValue(StrId nameId, std::function<uint8_t()> getter, std::function<void(uint8_t)> setter,
                                  const ValueRange valueRange, const char* key = nullptr,
                                  StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.valueGetter = std::move(getter);
    s.valueSetter = std::move(setter);
    s.valueRange = valueRange;
    s.key = key;
    s.category = category;
    return s;
  }

  // ACTION row dispatched via a caller-supplied handler rather than a
  // SettingAction case (see SettingAction::Extension).
  static SettingInfo ExtensionAction(std::function<void(Activity&)> handler) {
    SettingInfo s;
    s.type = SettingType::ACTION;
    s.action = SettingAction::Extension;
    s.actionHandler = std::move(handler);
    return s;
  }
};

// A downstream integration's set of extra rows, grouped under one Settings
// tab. See SettingsExtension.h for how a provider supplies these.
struct SettingsExtensionCategory {
  std::string label;  // pre-localized by the provider; shown as the tab title
  std::vector<SettingInfo> settings;
  // Card artwork on the touch category landing screen (see
  // SettingsCategoryGridLayout.h). Optional: nullptr draws the generic
  // extra-category icon, so a provider that only wants rows supplies nothing.
  // Ignored by the tab band, which is text-only.
  const freeink::Icon* icon = nullptr;
};

// Index arithmetic for a tab band made of built-in categories followed by an
// extension provider's categories. A provider is re-consulted on every rebuild
// and may return a different number of categories each time (see
// SettingsExtension.h), so the band's size is not fixed once the screen has
// been entered. Everything that indexes the band -- the tab count, the label
// lookup, the remembered active tab, and the host's per-tab navigation state --
// has to be derived from the same, current range; restating the arithmetic at
// each use site is what lets one of them index past the others.
struct SettingsTabRange {
  int builtInCount = 0;
  int extraCount = 0;

  [[nodiscard]] constexpr int count() const { return builtInCount + extraCount; }
  [[nodiscard]] constexpr bool isExtension(const int index) const { return index >= builtInCount; }
  // Position within the provider's categories; meaningful only when isExtension().
  [[nodiscard]] constexpr int extraIndex(const int index) const { return index - builtInCount; }

  // Keeps a remembered tab addressable after the provider grew or shrank.
  [[nodiscard]] constexpr int clamp(const int index) const {
    const int last = count() - 1;
    if (last < 0) return 0;
    if (index < 0) return 0;
    return index > last ? last : index;
  }

  // Resizes per-tab state to one entry per tab, preserving the entries of the
  // tabs that survive. Must run on every rebuild, not only on screen entry:
  // state sized against a stale count is exactly one entry short of the tab a
  // grown provider just made selectable.
  template <typename TabState>
  void sizeTabState(std::vector<TabState>& state) const {
    const int wanted = count() < 0 ? 0 : count();
    if (state.size() != static_cast<size_t>(wanted)) state.resize(static_cast<size_t>(wanted));
  }
};
