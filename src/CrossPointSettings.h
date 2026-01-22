#pragma once
#include <array>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

// Setting descriptor infrastructure
enum class SettingType { TOGGLE, ENUM, VALUE, STRING };

// Validator function pointer (not std::function to save memory)
using SettingValidator = bool (*)(uint8_t);

// Forward declare for descriptors
class CrossPointSettings;

// Forward declare file type
class FsFile;

// Base descriptor for all settings (non-virtual for constexpr)
struct SettingDescriptorBase {
  const char* name;  // Display name
  SettingType type;
};

// Value range for VALUE type settings
struct ValueRange {
  uint8_t min, max, step;
};

// Concrete descriptor for uint8_t settings (constexpr-compatible)
struct SettingDescriptor : public SettingDescriptorBase {
  union {
    uint8_t CrossPointSettings::* memberPtr;  // For TOGGLE/ENUM/VALUE types
    char* stringPtr;                          // For STRING type
  };
  uint8_t defaultValue;
  SettingValidator validator;  // Optional validator function

  union {
    // For ENUM types
    struct {
      const char* const* values;
      uint8_t count;
    } enumData;

    // For VALUE types
    ValueRange valueRange;

    // For STRING types
    struct {
      const char* defaultString;  // Default string value
      size_t maxSize;             // Max size of the string buffer
    } stringData;
  };

  // Constexpr constructors for different setting types
  // TOGGLE/ENUM constructor
  constexpr SettingDescriptor(const char* name_, SettingType type_, uint8_t CrossPointSettings::* ptr, uint8_t defVal,
                              SettingValidator val, const char* const* enumVals, uint8_t enumCnt)
      : SettingDescriptorBase{name_, type_},
        memberPtr(ptr),
        defaultValue(defVal),
        validator(val),
        enumData{enumVals, enumCnt} {}

  // VALUE constructor
  constexpr SettingDescriptor(const char* name_, SettingType type_, uint8_t CrossPointSettings::* ptr, uint8_t defVal,
                              SettingValidator val, ValueRange valRange)
      : SettingDescriptorBase{name_, type_},
        memberPtr(ptr),
        defaultValue(defVal),
        validator(val),
        valueRange(valRange) {}

  // STRING constructor
  constexpr SettingDescriptor(const char* name_, SettingType type_, char* strPtr, const char* defStr, size_t maxSz)
      : SettingDescriptorBase{name_, type_},
        stringPtr(strPtr),
        defaultValue(0),
        validator(nullptr),
        stringData{defStr, maxSz} {}

  bool validate(const CrossPointSettings& settings) const;
  uint8_t getValue(const CrossPointSettings& settings) const;
  void setValue(CrossPointSettings& settings, uint8_t value) const;
  void resetToDefault(CrossPointSettings& settings) const;
  void save(FsFile& file, const CrossPointSettings& settings) const;
  void load(FsFile& file, CrossPointSettings& settings) const;

  // Helper to get enum value as string
  const char* getEnumValueString(uint8_t index) const {
    if (index < enumData.count && enumData.values) {
      return enumData.values[index];
    }
    return "";
  }
};

// Validator functions (constexpr for compile-time optimization)
constexpr bool validateToggle(uint8_t v) { return v <= 1; }
template <uint8_t MAX>
constexpr bool validateEnum(uint8_t v) {
  return v < MAX;
}
template <uint8_t MIN, uint8_t MAX>
constexpr bool validateRange(uint8_t v) {
  return v >= MIN && v <= MAX;
}

class CrossPointSettings {
 private:
  // Private constructor for singleton
  CrossPointSettings() = default;

  // Static instance
  static CrossPointSettings instance;

 public:
  // Delete copy constructor and assignment
  CrossPointSettings(const CrossPointSettings&) = delete;
  CrossPointSettings& operator=(const CrossPointSettings&) = delete;

  // Static constexpr array of all setting descriptors
  static constexpr size_t DESCRIPTOR_COUNT = 23;
  static const std::array<SettingDescriptor, DESCRIPTOR_COUNT> descriptors;

  // Should match with SettingsActivity text
  enum SLEEP_SCREEN_MODE { DARK = 0, LIGHT = 1, CUSTOM = 2, COVER = 3, BLANK = 4 };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1 };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
  };

  // Status bar display type enum
  enum STATUS_BAR_MODE {
    NONE = 0,
    NO_PROGRESS = 1,
    FULL = 2,
    FULL_WITH_PROGRESS_BAR = 3,
    ONLY_PROGRESS_BAR = 4,
  };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
  };

  // Front button layout options
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
  };

  // Side button layout options
  // Default: Previous, Next
  // Swapped: Next, Previous
  enum SIDE_BUTTON_LAYOUT { PREV_NEXT = 0, NEXT_PREV = 1 };

  // Font family options
  enum FONT_FAMILY { BOOKERLY = 0, NOTOSANS = 1, OPENDYSLEXIC = 2 };
  // Font size options
  enum FONT_SIZE { SMALL = 0, MEDIUM = 1, LARGE = 2, EXTRA_LARGE = 3 };
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2 };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
  };

  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
  };

  // E-ink refresh frequency (pages between full refreshes)
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
  };

  // Short power button press actions
  enum SHORT_PWRBTN { IGNORE = 0, SLEEP = 1, PAGE_TURN = 2 };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2 };

  // Sleep screen settings
  uint8_t sleepScreen = DARK;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Status bar settings
  uint8_t statusBar = FULL;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  uint8_t textAntiAliasing = 1;
  // Short power button click behaviour
  uint8_t shortPwrBtn = IGNORE;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Button layouts
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  uint8_t sideButtonLayout = PREV_NEXT;
  // Reader font settings
  uint8_t fontFamily = BOOKERLY;
  uint8_t fontSize = MEDIUM;
  uint8_t lineSpacing = NORMAL;
  uint8_t paragraphAlignment = JUSTIFIED;
  // Auto-sleep timeout setting (default 10 minutes)
  uint8_t sleepTimeout = SLEEP_10_MIN;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margin settings
  uint8_t screenMargin = 5;
  // OPDS browser settings
  char opdsServerUrl[128] = "";
  char opdsUsername[64] = "";
  char opdsPassword[64] = "";
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // Long-press chapter skip on side buttons
  uint8_t longPressChapterSkip = 1;

  ~CrossPointSettings() = default;

  // Get singleton instance
  static CrossPointSettings& getInstance() { return instance; }

  uint16_t getPowerButtonDuration() const {
    return (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) ? 10 : 400;
  }
  int getReaderFontId() const;

  bool saveToFile() const;
  bool loadFromFile();

  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
