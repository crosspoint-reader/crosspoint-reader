#pragma once
#include <HalStorage.h>

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

  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    BLANK = 4,
    COVER_CUSTOM = 5,
    OVERLAY = 6,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_IMAGE_PICK_MODE { PICK_RANDOM = 0, PICK_SEQUENTIAL = 1, SLEEP_IMAGE_PICK_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };

  // Status bar enum - legacy
  enum STATUS_BAR_MODE {
    NONE = 0,
    NO_PROGRESS = 1,
    FULL = 2,
    BOOK_PROGRESS_BAR = 3,
    ONLY_BOOK_PROGRESS_BAR = 4,
    CHAPTER_PROGRESS_BAR = 5,
    STATUS_BAR_MODE_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR {
    BOOK_PROGRESS = 0,
    CHAPTER_PROGRESS = 1,
    HIDE_PROGRESS = 2,
    STATUS_BAR_PROGRESS_BAR_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR_THICKNESS {
    PROGRESS_BAR_THIN = 0,
    PROGRESS_BAR_NORMAL = 1,
    PROGRESS_BAR_THICK = 2,
    STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT
  };
  enum STATUS_BAR_TITLE { BOOK_TITLE = 0, CHAPTER_TITLE = 1, HIDE_TITLE = 2, STATUS_BAR_TITLE_COUNT };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };

  // Front button layout options (legacy)
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Font family options (built-in fonts only; SD card fonts use sdFontFamilyName)
  enum FONT_FAMILY { BOOKERLY = 0, NOTOSANS = 1, OPENDYSLEXIC = 2, FONT_FAMILY_COUNT };
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;
  // Font size options
  enum FONT_SIZE { SMALL = 0, MEDIUM = 1, LARGE = 2, EXTRA_LARGE = 3, FONT_SIZE_COUNT };
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2, LINE_COMPRESSION_COUNT };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };

  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_TIMEOUT_COUNT
  };

  // E-ink refresh frequency (pages between full refreshes)
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_FREQUENCY_COUNT
  };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // UI Theme
  enum UI_THEME { CLASSIC = 0, LYRA = 1, LYRA_3_COVERS = 2 };

  // Image rendering in EPUB reader
  enum IMAGE_RENDERING { IMAGES_DISPLAY = 0, IMAGES_PLACEHOLDER = 1, IMAGES_SUPPRESS = 2, IMAGE_RENDERING_COUNT };

  // Text darkness for AA glyph rendering (forwarded to GfxRenderer::setTextDarkness)
  enum TEXT_DARKNESS {
    DARKNESS_NORMAL = 0,      // true 4-level AA
    DARKNESS_DARK = 1,        // historical default — bolder dark-gray AA
    DARKNESS_EXTRA_DARK = 2,  // both AA shades pushed to darkest gray state
    DARKNESS_MAXIMUM = 3,     // skip grayscale passes — AA pixels stay solid black from BW pass
    TEXT_DARKNESS_COUNT
  };
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  enum IMAGE_DITHERING {
    IMAGE_DITHER_BAYER = 0,
    IMAGE_DITHER_ATKINSON = 1,
    IMAGE_DITHER_DIFFUSED_BAYER = 2,
    IMAGE_DITHERING_COUNT
  };
#else
  enum IMAGE_DITHERING { IMAGE_DITHER_BAYER = 0, IMAGE_DITHERING_COUNT };
#endif

  // Timezone options (POSIX TZ rules for DST support)
  enum TIMEZONE {
    TZ_UTC = 0,
    TZ_CET = 1,
    TZ_EET = 2,
    TZ_MSK = 3,
    TZ_UTC_PLUS4 = 4,
    TZ_IST = 5,
    TZ_UTC_PLUS7 = 6,
    TZ_UTC_PLUS8 = 7,
    TZ_UTC_PLUS9 = 8,
    TZ_AEST = 9,
    TZ_NZST = 10,
    TZ_UTC_MINUS3 = 11,
    TZ_EST = 12,
    TZ_CST = 13,
    TZ_MST = 14,
    TZ_PST = 15,
    TZ_AST_ADT = 16,
    TZ_ACST_ACDT = 17,
    TZ_AKST_AKDT = 18,
    TIMEZONE_COUNT
  };

  // Sleep screen settings
  uint8_t sleepScreen = DARK;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Apply information overlay with reading progress on sleep cover
  uint8_t sleepCoverOverlay = 0;
  // Sleep image pick mode (random vs sequential walk-through)
  uint8_t sleepImagePickMode = PICK_RANDOM;
  // Status bar settings (statusBar retained for migration only)
  uint8_t statusBar = FULL;
  uint8_t statusBarChapterPageCount = 1;
  uint8_t statusBarBookProgressPercentage = 1;
  uint8_t statusBarProgressBar = HIDE_PROGRESS;
  uint8_t statusBarProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarTitle = CHAPTER_TITLE;
  uint8_t statusBarBattery = 1;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  uint8_t textAntiAliasing = 1;
  // Text darkness (0 = normal, 1 = dark, 2 = extra dark). Default 1 preserves
  // historical AA rendering (both grayscale shades drawn in the MSB pass).
  uint8_t textDarkness = DARKNESS_DARK;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Button layouts (front layout retained for migration only)
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  // Front button remap (logical -> hardware)
  // Used by MappedInputManager to translate logical buttons into physical front buttons.
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // Reader font settings
  uint8_t fontFamily = BOOKERLY;
  // SD card font family name (empty = use built-in fontFamily)
  char sdFontFamilyName[32] = "";
  uint8_t fontSize = MEDIUM;
  uint8_t lineSpacing = NORMAL;
  uint8_t paragraphAlignment = JUSTIFIED;
  // Auto-sleep timeout setting (default 10 minutes)
  uint8_t sleepTimeout = SLEEP_10_MIN;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;
  // Perform a half refresh on the page immediately following an EPUB page that displayed images.
  uint8_t halfRefreshAfterImagePage = 1;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margin settings
  uint8_t screenMargin = 5;
  // OPDS browser settings
  char opdsServerUrl[128] = "";
  char opdsUsername[64] = "";
  char opdsPassword[64] = "";
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // UI Theme
  uint8_t uiTheme = LYRA;
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Use book's embedded CSS styles for EPUB rendering (1 = enabled, 0 = disabled)
  uint8_t embeddedStyle = 1;
  // Show hidden files/directories (starting with '.') in the file browser (0 = hidden, 1 = show)
  uint8_t showHiddenFiles = 0;
  // Show file extensions in the file browser (0 = hidden, 1 = show)
  uint8_t showFileExtensions = 0;
  // Image rendering mode in EPUB reader
  uint8_t imageRendering = IMAGES_DISPLAY;
  // Dithering mode for decoded images (EPUB/JPG/PNG)
  uint8_t imageDithering = IMAGE_DITHER_BAYER;
  // Enable synthetic TOC fallback for malformed/sparse TOC books (1 = enabled, 0 = disabled)
  uint8_t syntheticTocFallback = 1;
  // Automatically push reading progress to the KOReader sync server when leaving the reader
  // (1 = enabled, 0 = disabled). The push only fires when credentials are configured and the
  // reader session advanced at least 3 pages, and is skipped when remote progress is already ahead.
  uint8_t koSyncOnBookClose = 0;
  // Show clock in the reader status bar
  uint8_t statusBarClock = 0;
  // Clock format: 0 = 24h (14:00), 1 = 12h (2:00pm)
  uint8_t clockFormat12h = 0;
  // Timezone selection (applies POSIX TZ rules for DST)
  uint8_t timeZone = TZ_UTC;
  // Use clock and keep the LP timer running during deep sleep (GPIO13 HIGH)
  // so time can be accurately restored on wake. Increases sleep current by ~3-4 mA.
  uint8_t useClock = 0;
  // Show the Weather home screen menu item (1 = enabled, 0 = hidden)
  uint8_t useWeather = 1;

  // Configurable actions for short / double / long press on each logical button.
  // BTN_DEFAULT means "use the button's normal built-in behaviour".
  enum BUTTON_ACTION {
    BTN_DEFAULT = 0,
    BTN_PAGE_FORWARD,
    BTN_PAGE_BACK,
    BTN_PAGE_FORWARD_10,
    BTN_PAGE_BACK_10,
    BTN_GO_HOME,
    BTN_SLEEP,
    BTN_FORCE_REFRESH,
    BTN_OPEN_TOC,
    BTN_OPEN_BOOKMARKS,
    BTN_STAR_PAGE,
    BTN_FOOTNOTES,
    BTN_NEXT_SECTION,
    BTN_PREV_SECTION,
    BTN_EXIT_READER,
    BTN_READER_MENU,
    BTN_KOREADER_SYNC,
    BUTTON_ACTION_COUNT
  };

  // Short-press actions (default: built-in)
  uint8_t btnShortBack = BTN_DEFAULT;
  uint8_t btnShortConfirm = BTN_DEFAULT;
  uint8_t btnShortLeft = BTN_DEFAULT;
  uint8_t btnShortRight = BTN_DEFAULT;
  uint8_t btnShortPageBack = BTN_DEFAULT;
  uint8_t btnShortPageForward = BTN_DEFAULT;
  uint8_t btnShortPower = BTN_DEFAULT;

  // Double-press actions (default: BTN_DEFAULT = disabled, no disambiguation wait)
  uint8_t btnDoubleBack = BTN_DEFAULT;
  uint8_t btnDoubleConfirm = BTN_DEFAULT;
  uint8_t btnDoubleLeft = BTN_DEFAULT;
  uint8_t btnDoubleRight = BTN_DEFAULT;
  uint8_t btnDoublePageBack = BTN_DEFAULT;
  uint8_t btnDoublePageForward = BTN_DEFAULT;
  uint8_t btnDoublePower = BTN_DEFAULT;

  // Long-press actions (default: built-in)
  uint8_t btnLongBack = BTN_DEFAULT;
  uint8_t btnLongConfirm = BTN_DEFAULT;
  uint8_t btnLongLeft = BTN_DEFAULT;
  uint8_t btnLongRight = BTN_DEFAULT;
  uint8_t btnLongPageBack = BTN_DEFAULT;
  uint8_t btnLongPageForward = BTN_DEFAULT;
  uint8_t btnLongPower = BTN_DEFAULT;

  ~CrossPointSettings() = default;

  // Get singleton instance
  static CrossPointSettings& getInstance() { return instance; }

  static constexpr uint16_t getPowerButtonDuration() { return 400; }
  int getReaderFontId() const;
  // Pure built-in lookup (size enum + family enum -> font ID). Independent of
  // SD-card font selection. Used by the per-book fontFamilyOverride path so
  // an override forces back to a known built-in even when an SD font is the
  // global default.
  static int getBuiltinReaderFontId(uint8_t family, uint8_t size);

  // If count_only is true, returns the number of settings items that would be written.
  uint8_t writeSettings(FsFile& file, bool count_only = false) const;

  bool saveToFile() const;
  bool loadFromFile();

  static void validateFrontButtonMapping(CrossPointSettings& settings);

 private:
  bool loadFromBinaryFile();

 public:
  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
