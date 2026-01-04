#pragma once
#include <cstdint>
#include <iosfwd>
#include <string>

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

  // Default folder options
  enum DEFAULT_FOLDER { FOLDER_ROOT = 0, FOLDER_CUSTOM = 1, FOLDER_LAST_USED = 2 };

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
  // Bluetooth settings
  uint8_t bluetoothEnabled = 0;
  // File browser settings
  uint8_t useCoverArtPicker = 0;
  // Auto-sleep timeout (enum index: 0=2min, 1=5min, 2=10min, 3=15min, 4=20min, 5=30min, 6=60min, 7=Never)
  uint8_t autoSleepMinutes = 1;  // Default to 5 minutes
  // Screen refresh interval (enum index: 0=1pg, 1=3pg, 2=5pg, 3=10pg, 4=15pg, 5=20pg)
  uint8_t refreshInterval = 4;  // Default to 15 pages (current behavior)
  // Default folder for file browser (enum index: 0=Root, 1=Custom, 2=Last Used)
  uint8_t defaultFolder = FOLDER_LAST_USED;  // Default to last used (current behavior)

  // Schedule settings for auto-starting file transfer server
  uint8_t scheduleEnabled = 0;       // 0=disabled, 1=enabled
  uint8_t scheduleFrequency = 0;     // 0=1hr, 1=2hr, 2=3hr, 3=6hr, 4=12hr, 5=24hr, 6=Scheduled time
  uint8_t scheduleProtocol = 0;      // 0=HTTP, 1=FTP
  uint8_t scheduleNetworkMode = 0;   // 0=Join Network, 1=Create Hotspot
  uint8_t scheduleHour = 0;          // 0-23: Hour of day for scheduled start (when scheduleFrequency=6)
  uint8_t scheduleAutoShutdown = 2;  // 0=5min, 1=10min, 2=20min, 3=30min, 4=60min, 5=120min

  // Custom default folder path (used when defaultFolder == FOLDER_CUSTOM)
  std::string customDefaultFolder = "/books";

  // Network credentials
  std::string ftpUsername = "crosspoint";
  std::string ftpPassword = "reader";
  std::string httpUsername = "crosspoint";
  std::string httpPassword = "reader";
  std::string apSsid = "CrossPoint-Reader";
  std::string apPassword = "";  // Empty = open network

  ~CrossPointSettings() = default;

  // Get singleton instance
  static CrossPointSettings& getInstance() { return instance; }

  uint16_t getPowerButtonDuration() const { return shortPwrBtn ? 10 : 400; }
  int getReaderFontId() const;
  unsigned long getAutoSleepTimeoutMs() const {
    // Map enum index to milliseconds: 0=2min, 1=5min, 2=10min, 3=15min, 4=20min, 5=30min, 6=60min, 7=Never(0)
    constexpr unsigned long timeouts[] = {
        2UL * 60UL * 1000UL,   // 0: 2 minutes
        5UL * 60UL * 1000UL,   // 1: 5 minutes (default)
        10UL * 60UL * 1000UL,  // 2: 10 minutes
        15UL * 60UL * 1000UL,  // 3: 15 minutes
        20UL * 60UL * 1000UL,  // 4: 20 minutes
        30UL * 60UL * 1000UL,  // 5: 30 minutes
        60UL * 60UL * 1000UL,  // 6: 60 minutes
        0UL                    // 7: Never (disabled)
    };
    return (autoSleepMinutes < 8) ? timeouts[autoSleepMinutes] : timeouts[2];
  }

  int getRefreshIntervalPages() const {
    // Map enum index to pages: 0=1, 1=3, 2=5, 3=10, 4=15 (default), 5=20
    constexpr int intervals[] = {1, 3, 5, 10, 15, 20};
    return (refreshInterval < 6) ? intervals[refreshInterval] : 15;
  }

  const char* getDefaultFolderPath() const {
    // Returns the configured default folder path (doesn't handle FOLDER_LAST_USED)
    if (defaultFolder == FOLDER_ROOT) return "/";
    if (defaultFolder == FOLDER_CUSTOM) return customDefaultFolder.c_str();
    return "/";  // Fallback
  }

  unsigned long getScheduleIntervalMs() const {
    // Map enum index to milliseconds: 0=1hr, 1=2hr, 2=3hr, 3=6hr, 4=12hr, 5=24hr, 6=Scheduled
    constexpr unsigned long intervals[] = {
        1UL * 60UL * 60UL * 1000UL,   // 0: 1 hour
        2UL * 60UL * 60UL * 1000UL,   // 1: 2 hours
        3UL * 60UL * 60UL * 1000UL,   // 2: 3 hours
        6UL * 60UL * 60UL * 1000UL,   // 3: 6 hours
        12UL * 60UL * 60UL * 1000UL,  // 4: 12 hours
        24UL * 60UL * 60UL * 1000UL,  // 5: 24 hours
        0UL                            // 6: Scheduled (use time-based check)
    };
    return (scheduleFrequency < 7) ? intervals[scheduleFrequency] : intervals[0];
  }

  unsigned long getAutoShutdownMs() const {
    // Map enum index to milliseconds: 0=5min, 1=10min, 2=20min, 3=30min, 4=60min, 5=120min
    constexpr unsigned long durations[] = {
        5UL * 60UL * 1000UL,    // 0: 5 minutes
        10UL * 60UL * 1000UL,   // 1: 10 minutes
        20UL * 60UL * 1000UL,   // 2: 20 minutes (default)
        30UL * 60UL * 1000UL,   // 3: 30 minutes
        60UL * 60UL * 1000UL,   // 4: 60 minutes
        120UL * 60UL * 1000UL   // 5: 120 minutes
    };
    return (scheduleAutoShutdown < 6) ? durations[scheduleAutoShutdown] : durations[2];
  }

  bool saveToFile() const;
  bool loadFromFile();

  float getReaderLineCompression() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
