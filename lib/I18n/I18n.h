#pragma once

#include <cstdint>

/**
 * Internationalization (i18n) system for CrossPoint Reader
 */

// String IDs - organized by category
enum class StrId : uint16_t {
  // === Boot/Sleep ===
  CROSSPOINT, // "CrossPoint"
  BOOTING,    // "BOOTING"
  SLEEPING,   // "SLEEPING"
  ENTERING_SLEEP, // "Entering Sleep..."

  // === Home Menu ===
  BROWSE_FILES,     // "Browse Files"
  FILE_TRANSFER,    // "File Transfer"
  SETTINGS_TITLE,   // "Settings"
  CALIBRE_LIBRARY,  // "Calibre Library"
  CONTINUE_READING, // "Continue Reading"
  NO_OPEN_BOOK,     // "No open book"
  START_READING,    // "Start reading below"

  // === File Browser ===
  BOOKS,          // "Books"
  NO_BOOKS_FOUND, // "No books found"

  // === Reader ===
  SELECT_CHAPTER,  // "Select Chapter"
  NO_CHAPTERS,     // "No chapters"
  END_OF_BOOK,     // "End of book"
  EMPTY_CHAPTER,   // "Empty chapter"
  INDEXING,        // "Indexing..."
  MEMORY_ERROR,    // "Memory error"
  PAGE_LOAD_ERROR, // "Page load error"
  EMPTY_FILE,      // "Empty file"
  OUT_OF_BOUNDS,   // "Out of bounds"
  LOADING,         // "Loading..."
  LOAD_XTC_FAILED, // "Failed to load XTC"
  LOAD_TXT_FAILED, // "Failed to load TXT"
  LOAD_EPUB_FAILED, // "Failed to load EPUB"
  SD_CARD_ERROR,    // "SD card error"

  // === Network ===
  WIFI_NETWORKS,      // "WiFi Networks"
  NO_NETWORKS,        // "No networks found"
  NETWORKS_FOUND,     // "%zu networks found"
  SCANNING,           // "Scanning..."
  CONNECTING,         // "Connecting..."
  CONNECTED,          // "Connected!"
  CONNECTION_FAILED,  // "Connection Failed"
  CONNECTION_TIMEOUT, // "Connection timeout"
  FORGET_NETWORK,     // "Forget Network?"
  SAVE_PASSWORD,      // "Save password for next time?"
  REMOVE_PASSWORD,    // "Remove saved password?"
  PRESS_OK_SCAN,      // "Press OK to scan again"
  PRESS_ANY_CONTINUE, // "Press any button to continue"
  SELECT_HINT,  // "LEFT/RIGHT: Select | OK: Confirm"
  HOW_CONNECT,  // "How would you like to connect?"
  JOIN_NETWORK, // "Join a Network"
  CREATE_HOTSPOT, // "Create Hotspot"
  JOIN_DESC,    // "Connect to an existing WiFi network"
  HOTSPOT_DESC, // "Create a WiFi network others can join"
  STARTING_HOTSPOT,  // "Starting Hotspot..."
  HOTSPOT_MODE,      // "Hotspot Mode"
  CONNECT_WIFI_HINT, // "Connect your device to this WiFi network"
  OPEN_URL_HINT,     // "Open this URL in your browser"
  OR_HTTP_PREFIX,    // "or http://"
  SCAN_QR_HINT, // "or scan QR code with your phone:"
  CALIBRE_WIRELESS, // "Calibre Wireless"
  CALIBRE_WEB_URL,  // "Calibre Web URL"
  CONNECT_WIRELESS, // "Connect as Wireless Device"
  NETWORK_LEGEND,   // "* = Encrypted | + = Saved"
  MAC_ADDRESS,      // "MAC address:"
  CHECKING_WIFI,    // "Checking WiFi..."
  ENTER_WIFI_PASSWORD, // "Enter WiFi Password"
  ENTER_TEXT,       // "Enter Text"
  TO_PREFIX,        // "to "

  // === Calibre Wireless ===
  CALIBRE_DISCOVERING,          // "Discovering Calibre..."
  CALIBRE_CONNECTING_TO,        // "Connecting to "
  CALIBRE_CONNECTED_TO,         // "Connected to "
  CALIBRE_WAITING_COMMANDS,     // "Waiting for commands..."
  CONNECTION_FAILED_RETRYING,   // "(Connection failed, retrying)"
  CALIBRE_DISCONNECTED,         // "Calibre disconnected"
  CALIBRE_WAITING_TRANSFER,     // "Waiting for transfer..."
  CALIBRE_TRANSFER_HINT,        // Transfer hint text
  CALIBRE_RECEIVING,            // "Receiving: "
  CALIBRE_RECEIVED,             // "Received: "
  CALIBRE_WAITING_MORE,         // "Waiting for more..."
  CALIBRE_FAILED_CREATE_FILE,   // "Failed to create file"
  CALIBRE_PASSWORD_REQUIRED,    // "Password required"
  CALIBRE_TRANSFER_INTERRUPTED, // "Transfer interrupted"
  CALIBRE_INSTRUCTION_1,        // "1) Install CrossPoint Reader plugin"
  CALIBRE_INSTRUCTION_2,        // "2) Be on the same WiFi network"
  CALIBRE_INSTRUCTION_3,        // "3) In Calibre: \"Send to device\""
  CALIBRE_INSTRUCTION_4,        // "Keep this screen open while sending"

  // === Settings Categories ===
  CAT_DISPLAY,      // "Display"
  CAT_READER,       // "Reader"
  CAT_CONTROLS,     // "Controls"
  CAT_SYSTEM,       // "System"

  // === Settings ===
  SLEEP_SCREEN,     // "Sleep Screen"
  SLEEP_COVER_MODE, // "Sleep Screen Cover Mode"
  STATUS_BAR,       // "Status Bar"
  HIDE_BATTERY,     // "Hide Battery %"
  EXTRA_SPACING,    // "Extra Paragraph Spacing"
  TEXT_AA,          // "Text Anti-Aliasing"
  SHORT_PWR_BTN,    // "Short Power Button Click"
  ORIENTATION,      // "Reading Orientation"
  FRONT_BTN_LAYOUT, // "Front Button Layout"
  SIDE_BTN_LAYOUT,  // "Side Button Layout (reader)"
  LONG_PRESS_SKIP,  // "Long-press Chapter Skip"
  FONT_FAMILY,      // "Reader Font Family"
  EXT_READER_FONT,  // "External Reader Font"
  EXT_CHINESE_FONT, // "Reader Font"
  EXT_UI_FONT,      // "External UI Font"
  FONT_SIZE,        // "Reader Font Size"
  LINE_SPACING,     // "Reader Line Spacing"
  ASCII_LETTER_SPACING, // "ASCII Letter Spacing"
  ASCII_DIGIT_SPACING,  // "ASCII Digit Spacing"
  CJK_SPACING,          // "CJK Spacing"
  COLOR_MODE,       // "Color Mode"
  SCREEN_MARGIN,    // "Reader Screen Margin"
  PARA_ALIGNMENT,   // "Reader Paragraph Alignment"
  HYPHENATION,      // "Hyphenation"
  TIME_TO_SLEEP,    // "Time to Sleep"
  REFRESH_FREQ,     // "Refresh Frequency"
  CALIBRE_SETTINGS, // "Calibre Settings"
  KOREADER_SYNC,    // "KOReader Sync"
  CHECK_UPDATES,    // "Check for updates"
  LANGUAGE,         // "Language"
  SELECT_WALLPAPER, // "Select Wallpaper"
  CLEAR_READING_CACHE, // "Clear Reading Cache"

  // === Calibre Settings ===
  CALIBRE, // "Calibre"

  // === KOReader Settings ===
  USERNAME,            // "Username"
  PASSWORD,            // "Password"
  SYNC_SERVER_URL,     // "Sync Server URL"
  DOCUMENT_MATCHING,   // "Document Matching"
  AUTHENTICATE,        // "Authenticate"
  KOREADER_USERNAME,   // "KOReader Username"
  KOREADER_PASSWORD,   // "KOReader Password"
  FILENAME,            // "Filename"
  BINARY,              // "Binary"
  SET_CREDENTIALS_FIRST, // "Set credentials first"

  // === KOReader Auth ===
  WIFI_CONN_FAILED,  // "WiFi connection failed"
  AUTHENTICATING,    // "Authenticating..."
  AUTH_SUCCESS,      // "Successfully authenticated!"
  KOREADER_AUTH,     // "KOReader Auth"
  SYNC_READY,        // "KOReader sync is ready to use"
  AUTH_FAILED,       // "Authentication Failed"
  DONE,              // "Done"

  // === Clear Cache ===
  CLEAR_CACHE_WARNING_1, // "This will clear all cached book data."
  CLEAR_CACHE_WARNING_2, // "All reading progress will be lost!"
  CLEAR_CACHE_WARNING_3, // "Books will need to be re-indexed"
  CLEAR_CACHE_WARNING_4, // "when opened again."
  CLEARING_CACHE,        // "Clearing cache..."
  CACHE_CLEARED,         // "Cache Cleared"
  ITEMS_REMOVED,         // "items removed"
  FAILED_LOWER,          // "failed"
  CLEAR_CACHE_FAILED,    // "Failed to clear cache"
  CHECK_SERIAL_OUTPUT,   // "Check serial output for details"

  // Setting Values
  DARK,          // "Dark"
  LIGHT,         // "Light"
  CUSTOM,        // "Custom"
  COVER,         // "Cover"
  NONE_OPT,          // "None"
  FIT,           // "Fit"
  CROP,          // "Crop"
  NO_PROGRESS,   // "No Progress"
  FULL_OPT,          // "Full"
  NEVER,         // "Never"
  IN_READER,     // "In Reader"
  ALWAYS,        // "Always"
  IGNORE,        // "Ignore"
  SLEEP,         // "Sleep"
  PAGE_TURN,     // "Page Turn"
  PORTRAIT,      // "Portrait"
  LANDSCAPE_CW,  // "Landscape CW"
  INVERTED,      // "Inverted"
  LANDSCAPE_CCW, // "Landscape CCW"
  FRONT_LAYOUT_BCLR, // "Bck, Cnfrm, Lft, Rght"
  FRONT_LAYOUT_LRBC, // "Lft, Rght, Bck, Cnfrm"
  FRONT_LAYOUT_LBCR, // "Lft, Bck, Cnfrm, Rght"
  PREV_NEXT,     // "Prev/Next"
  NEXT_PREV,     // "Next/Prev"
  BOOKERLY,      // "Bookerly"
  NOTO_SANS,     // "Noto Sans"
  OPEN_DYSLEXIC, // "Open Dyslexic"
  SMALL,         // "Small"
  MEDIUM,        // "Medium"
  LARGE,         // "Large"
  X_LARGE,       // "X Large"
  TIGHT,         // "Tight"
  NORMAL,        // "Normal"
  WIDE,          // "Wide"
  JUSTIFY,       // "Justify"
  ALIGN_LEFT,          // "Left"
  CENTER,        // "Center"
  ALIGN_RIGHT,         // "Right"
  MIN_1,         // "1 min"
  MIN_5,         // "5 min"
  MIN_10,        // "10 min"
  MIN_15,        // "15 min"
  MIN_30,        // "30 min"
  PAGES_1,       // "1 page"
  PAGES_5,       // "5 pages"
  PAGES_10,      // "10 pages"
  PAGES_15,      // "15 pages"
  PAGES_30,      // "30 pages"

  // === OTA Update ===
  UPDATE,          // "Update"
  CHECKING_UPDATE, // "Checking for update..."
  NEW_UPDATE,      // "New update available!"
  CURRENT_VERSION, // "Current Version: "
  NEW_VERSION,     // "New Version: "
  UPDATING,        // "Updating..."
  NO_UPDATE,       // "No update available"
  UPDATE_FAILED,   // "Update failed"
  UPDATE_COMPLETE, // "Update complete"
  POWER_ON_HINT,   // "Press and hold power button to turn back on"

  // === Font Selection ===
  EXTERNAL_FONT,    // "External Font"
  BUILTIN_DISABLED, // "Built-in (Disabled)"

  // === OPDS Browser ===
  NO_ENTRIES,        // "No entries found"
  DOWNLOADING,       // "Downloading..."
  DOWNLOAD_FAILED,   // "Download failed"
  ERROR_MSG,             // "Error:"
  UNNAMED,           // "Unnamed"
  NO_SERVER_URL,     // "No server URL configured"
  FETCH_FEED_FAILED, // "Failed to fetch feed"
  PARSE_FEED_FAILED, // "Failed to parse feed"
  NETWORK_PREFIX,    // "Network: "
  IP_ADDRESS_PREFIX, // "IP Address: "
  SCAN_QR_WIFI_HINT, // "or scan QR code with your phone to connect to Wifi."
  ERROR_GENERAL_FAILURE, // "Error: General failure"
  ERROR_NETWORK_NOT_FOUND, // "Error: Network not found"
  ERROR_CONNECTION_TIMEOUT, // "Error: Connection timeout"
  SD_CARD,           // "SD card"

  // === Buttons ===
  BACK,    // "« Back"
  EXIT,    // "« Exit"
  HOME,    // "« Home"
  SAVE,    // "« Save"
  SELECT,  // "Select"
  TOGGLE,  // "Toggle"
  CONFIRM, // "Confirm"
  CANCEL,  // "Cancel"
  CONNECT, // "Connect"
  OPEN,    // "Open"
  DOWNLOAD, // "Download"
  RETRY,   // "Retry"
  YES,     // "Yes"
  NO,      // "No"
  STATE_ON,      // "ON"
  STATE_OFF,     // "OFF"
  SET,     // "Set"
  NOT_SET, // "Not Set"
  DIR_LEFT,  // "Left"
  DIR_RIGHT, // "Right"
  DIR_UP,    // "Up"
  DIR_DOWN,  // "Down"
  CAPS_ON,   // "CAPS"
  CAPS_OFF,  // "caps"
  OK_BUTTON, // "OK"

  // === Languages ===
  ENGLISH,          // "English"
  SPANISH,          // "Español"
  ITALIAN,          // "Italiano"
  SWEDISH,          // "Svenska"
  FRENCH,           // "Français"

  // Marker for current selection in LanguageSelectActivity
  ON_MARKER,        // "[ON]"

  // Master branch specific additions
  SLEEP_COVER_FILTER, // "Sleep Screen Cover Filter"
  FILTER_CONTRAST,    // "Contrast"
  
  STATUS_BAR_FULL_PERCENT, // "Full w/ Percentage"
  STATUS_BAR_FULL_BOOK,    // "Full w/ Book Bar"
  STATUS_BAR_BOOK_ONLY,    // "Book Bar Only"
  STATUS_BAR_FULL_CHAPTER, // "Full w/ Chapter Bar"

  UI_THEME,      // "UI Theme"
  THEME_CLASSIC, // "Classic"
  THEME_LYRA,    // "Lyra"

  SUNLIGHT_FADING_FIX, // "Sunlight Fading Fix"
  
  REMAP_FRONT_BUTTONS, // "Remap Front Buttons"
  OPDS_BROWSER,        // "OPDS Browser"
  COVER_CUSTOM,        // "Cover + Custom"
  RECENTS,             // "Recents"
  MENU_RECENT_BOOKS,        // "Recent Books"
  NO_RECENT_BOOKS,     // "No recent books"
  CALIBRE_DESC,        // "Use Calibre wireless device transfers"
  FORGET_AND_REMOVE,   // "Forget network and remove saved password?"
  FORGET_BUTTON,       // "Forget network"
  CALIBRE_STARTING,    // "Starting Calibre..."
  CALIBRE_SETUP,       // "Setup"
  CALIBRE_STATUS,      // "Status"
  CLEAR_BUTTON,        // "Clear"
  DEFAULT_VALUE,       // "Default"
  REMAP_PROMPT,        // "Press a front button for each role"
  UNASSIGNED,          // "Unassigned"
  ALREADY_ASSIGNED,    // "Already assigned"
  REMAP_RESET_HINT,    // "Side button Up: Reset to default layout"
  REMAP_CANCEL_HINT,   // "Side button Down: Cancel remapping"
  HW_BACK_LABEL,       // "Back (1st button)"
  HW_CONFIRM_LABEL,    // "Confirm (2nd button)"
  HW_LEFT_LABEL,       // "Left (3rd button)"
  HW_RIGHT_LABEL,      // "Right (4th button)"
  GO_TO_PERCENT,       // "Go to %"
  GO_HOME_BUTTON,      // "Go Home"
  SYNC_PROGRESS,       // "Sync Progress"
  DELETE_CACHE,        // "Delete Book Cache"
  CHAPTER_PREFIX,      // "Chapter: "
  PAGES_SEPARATOR,     // " pages  |  "
  BOOK_PREFIX,         // "Book: "
  KBD_SHIFT,           // "shift"
  KBD_SHIFT_CAPS,      // "SHIFT"
  KBD_LOCK,            // "LOCK"
  CALIBRE_URL_HINT,    // "For Calibre, add /opds to your URL"
  PERCENT_STEP_HINT,   // "Left/Right: 1%  Up/Down: 10%"
  SYNCING_TIME,        // "Syncing time..."
  CALC_HASH,           // "Calculating document hash..."
  HASH_FAILED,         // "Failed to calculate document hash"
  FETCH_PROGRESS,      // "Fetching remote progress..."
  UPLOAD_PROGRESS,     // "Uploading progress..."
  NO_CREDENTIALS_MSG,  // "No credentials configured"
  KOREADER_SETUP_HINT, // "Set up KOReader account in Settings"
  PROGRESS_FOUND,      // "Progress found!"
  REMOTE_LABEL,        // "Remote:"
  LOCAL_LABEL,         // "Local:"
  PAGE_OVERALL_FORMAT, // "  Page %d, %.2f%% overall"
  PAGE_TOTAL_OVERALL_FORMAT, // "  Page %d/%d, %.2f%% overall"
  DEVICE_FROM_FORMAT,  // "  From: %s"
  APPLY_REMOTE,        // "Apply remote progress"
  UPLOAD_LOCAL,        // "Upload local progress"
  NO_REMOTE_MSG,       // "No remote progress found"
  UPLOAD_PROMPT,       // "Upload current position?"
  UPLOAD_SUCCESS,      // "Progress uploaded!"
  SYNC_FAILED_MSG,     // "Sync failed"
  SECTION_PREFIX,      // "Section "
  UPLOAD,              // "Upload"

  // Sentinel - must be last
  _COUNT
};

// Language enum
enum class Language : uint8_t {
  ENGLISH = 0,
  SPANISH = 1,
  ITALIAN = 2,
  SWEDISH = 3,
  FRENCH = 4,
  _COUNT
};

class I18n {
public:
  static I18n &getInstance();

  // Disable copy
  I18n(const I18n &) = delete;
  I18n &operator=(const I18n &) = delete;

  /**
   * Get localized string by ID
   */
  const char *get(StrId id) const;

  /**
   * Shorthand operator for get()
   */
  const char *operator[](StrId id) const { return get(id); }

  /**
   * Get/Set current language
   */
  Language getLanguage() const { return _language; }
  void setLanguage(Language lang);

  /**
   * Save/Load language setting
   */
  void saveSettings();
  void loadSettings();

  /**
   * Get all unique characters used in a specific language
   * Returns a sorted string of unique characters
   */
  static const char *getCharacterSet(Language lang);

private:
  I18n() : _language(Language::ENGLISH) {}

  Language _language;
};

// Convenience macros
#define i18n(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
