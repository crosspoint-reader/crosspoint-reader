#pragma once

#include <functional>
#include <string>
#include <vector>

#include "activities/ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

class CrossPointSettings;

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING };

enum class SettingAction {
  None,
  RemapFrontButtons,
  KOReaderSync,
  OPDSBrowser,
  Network,
  ClearCache,
  CheckForUpdates,
};

struct SettingInfo {
  const char* name;
  SettingType type;
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  std::vector<std::string> enumValues;
  SettingAction action = SettingAction::None;

  struct ValueRange {
    uint8_t min;
    uint8_t max;
    uint8_t step;
  };
  ValueRange valueRange = {};

  const char* key = nullptr;       // JSON API key (nullptr for ACTION types)
  const char* category = nullptr;  // Category for web UI grouping

  // Direct char[] string fields (for settings stored in CrossPointSettings)
  char* stringPtr = nullptr;
  size_t stringMaxLen = 0;

  // Dynamic accessors (for settings stored outside CrossPointSettings, e.g. KOReaderCredentialStore)
  std::function<uint8_t()> valueGetter;
  std::function<void(uint8_t)> valueSetter;
  std::function<std::string()> stringGetter;
  std::function<void(const std::string&)> stringSetter;

  static SettingInfo Toggle(const char* name, uint8_t CrossPointSettings::* ptr, const char* key = nullptr,
                            const char* category = nullptr) {
    SettingInfo s;
    s.name = name;
    s.type = SettingType::TOGGLE;
    s.valuePtr = ptr;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Enum(const char* name, uint8_t CrossPointSettings::* ptr, std::vector<std::string> values,
                          const char* key = nullptr, const char* category = nullptr) {
    SettingInfo s;
    s.name = name;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.enumValues = std::move(values);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Action(const char* name, SettingAction action) {
    SettingInfo s;
    s.name = name;
    s.type = SettingType::ACTION;
    s.action = action;
    return s;
  }

  static SettingInfo Value(const char* name, uint8_t CrossPointSettings::* ptr, const ValueRange valueRange,
                           const char* key = nullptr, const char* category = nullptr) {
    SettingInfo s;
    s.name = name;
    s.type = SettingType::VALUE;
    s.valuePtr = ptr;
    s.valueRange = valueRange;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo String(const char* name, char* ptr, size_t maxLen, const char* key = nullptr,
                            const char* category = nullptr) {
    SettingInfo s;
    s.name = name;
    s.type = SettingType::STRING;
    s.stringPtr = ptr;
    s.stringMaxLen = maxLen;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicEnum(const char* name, std::vector<std::string> values, std::function<uint8_t()> getter,
                                 std::function<void(uint8_t)> setter, const char* key = nullptr,
                                 const char* category = nullptr) {
    SettingInfo s;
    s.name = name;
    s.type = SettingType::ENUM;
    s.enumValues = std::move(values);
    s.valueGetter = std::move(getter);
    s.valueSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicString(const char* name, std::function<std::string()> getter,
                                   std::function<void(const std::string&)> setter, const char* key = nullptr,
                                   const char* category = nullptr) {
    SettingInfo s;
    s.name = name;
    s.type = SettingType::STRING;
    s.stringGetter = std::move(getter);
    s.stringSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }
};

class SettingsActivity final : public ActivityWithSubactivity {
  ButtonNavigator buttonNavigator;

  int selectedCategoryIndex = 0;  // Currently selected category
  int selectedSettingIndex = 0;
  int settingsCount = 0;

  // Per-category settings derived from shared list + device-only actions
  std::vector<SettingInfo> displaySettings;
  std::vector<SettingInfo> readerSettings;
  std::vector<SettingInfo> controlsSettings;
  std::vector<SettingInfo> systemSettings;
  const std::vector<SettingInfo>* currentSettings = nullptr;

  const std::function<void()> onGoHome;

  static constexpr int categoryCount = 4;
  static const char* categoryNames[categoryCount];

  void enterCategory(int categoryIndex);
  void toggleCurrentSetting();

 public:
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                            const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("Settings", renderer, mappedInput), onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
};
