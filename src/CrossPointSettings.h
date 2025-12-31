#pragma once
#include <cstdint>
#include <iosfwd>

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

  // Should match with SettingsActivity text
  enum SLEEP_SCREEN_MODE { DARK = 0, LIGHT = 1, CUSTOM = 2, COVER = 3 };

  // Status bar display type enum
  enum STATUS_BAR_MODE { NONE = 0, NO_PROGRESS = 1, FULL = 2 };

  enum ORIENTATION {
    PORTRAIT = 0,      // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,  // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,      // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3  // 800x480 logical coordinates, native panel orientation
  };

  // Front button layout options
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT { BACK_CONFIRM_LEFT_RIGHT = 0, LEFT_RIGHT_BACK_CONFIRM = 1, LEFT_BACK_CONFIRM_RIGHT = 2 };

  // Side button layout options
  // Default: Previous, Next
  // Swapped: Next, Previous
  enum SIDE_BUTTON_LAYOUT { PREV_NEXT = 0, NEXT_PREV = 1 };

  // Font family options
  enum FONT_FAMILY { BOOKERLY = 0, NOTOSANS = 1, OPENDYSLEXIC = 2 };
  // Font size options
  enum FONT_SIZE { SMALL = 0, MEDIUM = 1, LARGE = 2, EXTRA_LARGE = 3 };
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2 };
  enum PARAGRAPH_ALIGNMENT { JUSTIFIED = 0, LEFT_ALIGN = 1, CENTER_ALIGN = 2, RIGHT_ALIGN = 3 };

  // Sleep screen settings
  uint8_t sleepScreen = DARK;
  // Status bar settings
  uint8_t statusBar = FULL;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  // Duration of the power button press
  uint8_t shortPwrBtn = 0;
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

  ~CrossPointSettings() = default;

  // Get singleton instance
  static CrossPointSettings& getInstance() { return instance; }

  uint16_t getPowerButtonDuration() const { return shortPwrBtn ? 10 : 400; }
  int getReaderFontId() const;

  bool saveToFile() const;
  bool loadFromFile();

  float getReaderLineCompression() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
