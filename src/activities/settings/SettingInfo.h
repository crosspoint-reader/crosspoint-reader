#pragma once
#include <I18n.h>

#include <cassert>
#include <string>
#include <vector>

#include "CrossPointSettings.h"

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING };

enum class SettingAction {
  None,
  RemapFrontButtons,
  CustomiseStatusBar,
  ClockSettings,
  KOReaderSync,
  OPDSBrowser,
  Network,
  ClearCache,
  CheckForUpdates,
  Language,
  SystemInfo,
  DetectTimezone,
  SyncTime,
  Weather,
  Submenu,
};

struct SettingInfo {
  StrId nameId;
  SettingType type;
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  std::vector<StrId> enumValues;
  SettingAction action = SettingAction::None;

  struct ValueRange {
    uint8_t min;
    uint8_t max;
    uint8_t step;
  };
  ValueRange valueRange = {};

  const char* key = nullptr;             // JSON API key (nullptr for ACTION types)
  StrId category = StrId::STR_NONE_OPT;  // Category for web UI grouping
  bool obfuscated = false;               // Save/load via base64 obfuscation (passwords)

  // Direct char[] string fields (for settings stored in CrossPointSettings)
  size_t stringOffset = 0;
  size_t stringMaxLen = 0;

  // Dynamic accessors (for settings stored outside CrossPointSettings, e.g. KOReaderCredentialStore).
  // Function pointers + opaque context avoid the heap allocation of std::function. Stateless
  // lambdas pass ctx=nullptr; captures must be hand-written as trampoline functions. See
  // DynamicEnumCtx / DynamicStringCtx factories below.
  using ValueGetterFn = uint8_t (*)(void*);
  using ValueSetterFn = void (*)(void*, uint8_t);
  using StringGetterFn = std::string (*)(void*);
  using StringSetterFn = void (*)(void*, const std::string&);

  void* accessorCtx = nullptr;
  ValueGetterFn valueGetter = nullptr;
  ValueSetterFn valueSetter = nullptr;
  StringGetterFn stringGetter = nullptr;
  StringSetterFn stringSetter = nullptr;

  uint8_t callValueGetter() const {
    assert(valueGetter && "SettingInfo::callValueGetter requires a non-null valueGetter");
    return valueGetter(accessorCtx);
  }
  void callValueSetter(uint8_t v) const {
    assert(valueSetter && "SettingInfo::callValueSetter requires a non-null valueSetter");
    valueSetter(accessorCtx, v);
  }
  std::string callStringGetter() const {
    assert(stringGetter && "SettingInfo::callStringGetter requires a non-null stringGetter");
    return stringGetter(accessorCtx);
  }
  void callStringSetter(const std::string& v) const {
    assert(stringSetter && "SettingInfo::callStringSetter requires a non-null stringSetter");
    stringSetter(accessorCtx, v);
  }

  SettingInfo& withObfuscated() {
    obfuscated = true;
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

  // Stateless variant — getter/setter are free/static functions with no captured state.
  static SettingInfo DynamicEnum(StrId nameId, std::vector<StrId> values, ValueGetterFn getter, ValueSetterFn setter,
                                 const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.enumValues = std::move(values);
    s.valueGetter = getter;
    s.valueSetter = setter;
    s.key = key;
    s.category = category;
    return s;
  }

  // Context-carrying variant — trampolines receive `ctx` as first argument and cast it back to
  // their concrete owner type.
  static SettingInfo DynamicEnumCtx(StrId nameId, std::vector<StrId> values, void* ctx, ValueGetterFn getter,
                                    ValueSetterFn setter, const char* key = nullptr,
                                    StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s = DynamicEnum(nameId, std::move(values), getter, setter, key, category);
    s.accessorCtx = ctx;
    return s;
  }

  static SettingInfo DynamicString(StrId nameId, StringGetterFn getter, StringSetterFn setter,
                                   const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringGetter = getter;
    s.stringSetter = setter;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicStringCtx(StrId nameId, void* ctx, StringGetterFn getter, StringSetterFn setter,
                                      const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s = DynamicString(nameId, getter, setter, key, category);
    s.accessorCtx = ctx;
    return s;
  }

  static SettingInfo Separator(StrId nameId) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ACTION;
    s.isSeparator = true;
    return s;
  }

  bool isSeparator = false;
  StrId subcategory = StrId::STR_NONE_OPT;  // Triggers a separator row on first use and on change
  StrId submenu = StrId::STR_NONE_OPT;      // Routes item into a submenu; hidden from main list

  // Inserts a separator row in the parent tab when this item's subcategory first appears or changes.
  SettingInfo& withSubcategory(StrId sub) {
    subcategory = sub;
    return *this;
  }

  // Hides this item from the parent tab and places it inside a SettingsSubmenuActivity instead.
  // All items sharing the same submenu StrId are grouped under one placeholder entry.
  SettingInfo& withSubmenu(StrId sub) {
    submenu = sub;
    return *this;
  }

  // For internal use by SettingsActivity: placeholder entry that launches the submenu.
  static SettingInfo SubmenuEntry(StrId titleId) {
    SettingInfo s;
    s.nameId = titleId;
    s.type = SettingType::ACTION;
    s.action = SettingAction::Submenu;
    return s;
  }

  // Returns the localised title; separators are decorated automatically.
  [[nodiscard]] std::string getTitle() const;

  // Returns the current display value string (ON/OFF for toggles, enum label, numeric value, >>).
  [[nodiscard]] std::string getDisplayValue() const;

  // Toggles/cycles the underlying value for TOGGLE, ENUM, and VALUE types.
  // Does nothing for ACTION and STRING types (callers handle those separately).
  // Marked const because it mutates the external SETTINGS global (via valuePtr),
  // not the SettingInfo itself.
  void toggleValue() const;
};
