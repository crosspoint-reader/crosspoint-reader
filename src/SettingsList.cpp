#include "SettingsList.h"

#include <array>
#include <utility>

void forEachBaseSetting(SettingVisitor visit, void* context) {
  // Load and save walk this list, so every condition here must be fixed before
  // settings load. Runtime probes belong in getSettingsList().

  // Enum settings are persisted as numeric values. Assign these labels by enum
  // value so a reordered menu or enum cannot silently swap their behavior.
  static constexpr auto SLEEP_SCREEN_VALUES = [] {
    std::array<StrId, CrossPointSettings::SLEEP_SCREEN_MODE_COUNT> values{};
    values[CrossPointSettings::DARK] = StrId::STR_DARK;
    values[CrossPointSettings::LIGHT] = StrId::STR_LIGHT;
    values[CrossPointSettings::CUSTOM] = StrId::STR_CUSTOM;
    values[CrossPointSettings::COVER] = StrId::STR_COVER;
    values[CrossPointSettings::COVER_CUSTOM] = StrId::STR_COVER_CUSTOM;
    values[CrossPointSettings::BLANK] = StrId::STR_NONE_OPT;
    values[CrossPointSettings::QUICK_RESUME] = StrId::STR_QUICK_RESUME;
    values[CrossPointSettings::TRANSPARENT_CUSTOM] = StrId::STR_TRANSPARENT;
    values[CrossPointSettings::COVER_TRANSPARENT] = StrId::STR_COVER_TRANSPARENT;
    return values;
  }();
  static constexpr auto STATUS_BAR_CLOCK_VALUES = [] {
    std::array<StrId, CrossPointSettings::STATUS_BAR_CLOCK_MODE_COUNT> values{};
    values[CrossPointSettings::STATUS_BAR_CLOCK_HIDE] = StrId::STR_HIDE;
    values[CrossPointSettings::STATUS_BAR_CLOCK_RIGHT] = StrId::STR_DIR_RIGHT;
    values[CrossPointSettings::STATUS_BAR_CLOCK_LEFT] = StrId::STR_DIR_LEFT;
    return values;
  }();

  const auto emit = [visit, context](auto&& info) { visit(context, info); };
  // --- Display ---
  emit(SettingInfo::StaticEnum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen, SLEEP_SCREEN_VALUES,
                               "sleepScreen", StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                         {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                         {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                         "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_QUICK_RESUME_TIMEOUT, &CrossPointSettings::quickResumeSleepScreen,
                         {StrId::STR_STATE_OFF, StrId::STR_STATE_ON}, "quickResumeSleepScreen",
                         StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                         {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                         StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
                         {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15,
                          StrId::STR_PAGES_30, StrId::STR_NEVER},
                         "refreshFrequency", StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_UI_THEME, &CrossPointSettings::uiTheme, homeThemeValues(), "uiTheme",
                         StrId::STR_CAT_DISPLAY));
  if (!BoardConfig::hasTouch())
    emit(SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                             StrId::STR_CAT_DISPLAY));
#if FREEINK_CAP_FRONTLIGHT
  emit(SettingInfo::Toggle(StrId::STR_RESTORE_LIGHT_ON_WAKE, &CrossPointSettings::frontlightRestoreOnWake,
                           "frontlightRestoreOnWake", StrId::STR_CAT_DISPLAY));
#endif
#if FREEINK_DEVICE_PICCO
  emit(SettingInfo::Enum(StrId::STR_SLIDE_SWITCH, &CrossPointSettings::slideSwitchAction,
                         {StrId::STR_FRONTLIGHT, StrId::STR_NIGHT_MODE, StrId::STR_TOUCH_READER_CONTROLS},
                         "slideSwitchAction", StrId::STR_CAT_DISPLAY));
#endif
  // Night mode = inverted output polarity everywhere (ActivityManager
  // applies it to every activity), so it lives in the Display category.
  emit(SettingInfo::Toggle(StrId::STR_NIGHT_MODE, &CrossPointSettings::screenInverted, "screenInverted",
                           StrId::STR_CAT_DISPLAY));

  // --- Reader ---
  // Built-in font-family entry. Replaced per-call with a registry-aware
  // version when SD fonts are installed.
  emit(SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily, {StrId::STR_NOTO_SANS}, "fontFamily",
                         StrId::STR_CAT_READER)
           .withTextSettings());
  // Placeholder: the selectable sizes depend on the active font family, so
  // UI callers replace this entry with buildFontSizeSetting(). It only
  // fixes the setting's position in the Reader category.
  emit(SettingInfo::Enum(StrId::STR_FONT_SIZE, nullptr, {}, "fontSize", StrId::STR_CAT_READER).withTextSettings());
  emit(SettingInfo::Enum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing,
                         {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE, StrId::STR_EXTRA_WIDE}, "lineSpacing",
                         StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Value(StrId::STR_WORD_SPACING, &CrossPointSettings::wordSpacing,
                          {CrossPointSettings::WORD_SPACING_MIN, CrossPointSettings::WORD_SPACING_MAX,
                           CrossPointSettings::WORD_SPACING_STEP},
                          "wordSpacing", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(StrId::STR_CHARACTER_SPACING, &CrossPointSettings::characterSpacing,
                         {StrId::STR_SPACING_MINUS_2, StrId::STR_SPACING_MINUS_1, StrId::STR_SPACING_ZERO,
                          StrId::STR_SPACING_PLUS_1, StrId::STR_SPACING_PLUS_2},
                         "characterSpacing", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin,
                          {CrossPointSettings::SCREEN_MARGIN_MIN, CrossPointSettings::SCREEN_MARGIN_MAX,
                           CrossPointSettings::SCREEN_MARGIN_STEP},
                          "screenMargin", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                         {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                          StrId::STR_BOOK_S_STYLE},
                         "paragraphAlignment", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                           StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Toggle(StrId::STR_FOCUS_READING, &CrossPointSettings::focusReadingEnabled, "focusReadingEnabled",
                           StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled",
                           StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(
      StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
      {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW},
      "orientation", StrId::STR_CAT_READER));
  emit(SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                           "extraParagraphSpacing", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Value(StrId::STR_PARAGRAPH_INDENTATION, &CrossPointSettings::paragraphIndentSpaces, {0, 5, 1},
                          "paragraphIndentSpaces", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                           StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                         {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                         "imageRendering", StrId::STR_CAT_READER));
  emit(SettingInfo::Enum(StrId::STR_READER_MENU_STYLE, &CrossPointSettings::readerMenuStyle,
                         {StrId::STR_MENU_STYLE_LIST, StrId::STR_MENU_STYLE_TOOLBAR}, "readerMenuStyle",
                         StrId::STR_CAT_READER));
  // --- Controls ---
#if FREEINK_CAP_HAPTIC
  emit(SettingInfo::Enum(StrId::STR_VIBRATION, &CrossPointSettings::vibration,
                         {StrId::STR_VIBRATION_TOUCH, StrId::STR_VIBRATION_TOUCH_PAGE, StrId::STR_STATE_OFF},
                         "vibration", StrId::STR_CAT_CONTROLS));
  emit(SettingInfo::Enum(StrId::STR_HAPTIC_INTENSITY, &CrossPointSettings::hapticIntensity,
                         {StrId::STR_HAPTIC_LOW, StrId::STR_HAPTIC_MEDIUM, StrId::STR_HAPTIC_HIGH}, "hapticIntensity",
                         StrId::STR_CAT_CONTROLS));
#endif
  emit(SettingInfo::Enum(
      StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
      {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV, StrId::STR_DISABLED, StrId::STR_NEXT_NEXT, StrId::STR_PREV_PREV},
      "sideButtonLayout", StrId::STR_CAT_CONTROLS));
  // The reader menu style stays available on button boards (the toolbar
  // chrome is button-navigable); only the touch controls are hidden.
  if (BoardConfig::hasTouch()) {
    emit(SettingInfo::Toggle(StrId::STR_TOUCH_READER_CONTROLS, &CrossPointSettings::touchReaderControls,
                             "touchReaderControls", StrId::STR_CAT_CONTROLS));
    emit(SettingInfo::Enum(StrId::STR_NEXT_PAGE_GESTURE, &CrossPointSettings::pageTurnGesture,
                           {StrId::STR_TAP_AND_SWIPE, StrId::STR_TAP_ONLY, StrId::STR_SWIPE_ONLY,
                            StrId::STR_INVERTED_TAP, StrId::STR_DISABLED},
                           "pageTurnGesture", StrId::STR_CAT_CONTROLS));
    emit(SettingInfo::Enum(StrId::STR_PREV_PAGE_GESTURE, &CrossPointSettings::previousPageGesture,
                           {StrId::STR_TAP_AND_SWIPE, StrId::STR_TAP_ONLY, StrId::STR_SWIPE_ONLY,
                            StrId::STR_INVERTED_TAP, StrId::STR_DISABLED},
                           "previousPageGesture", StrId::STR_CAT_CONTROLS));
  }
  // The reader-menu gesture choice only makes sense where the menu stays
  // reachable without the tap and the bottom edge is free (the capacitive
  // Home key); everywhere else the bottom-edge up-swipe is Home and the
  // center tap is the primary path, so the setting stays at its Tap default.
  // Persisted under the legacy "tapForReaderMenu" key: old saves map
  // 0 = Off, 1 = Tap.
  if (BoardConfig::hasHomeKey())
    emit(SettingInfo::Enum(StrId::STR_SHOW_READER_MENU, &CrossPointSettings::showReaderMenu,
                           {StrId::STR_STATE_OFF, StrId::STR_STATE_TAP, StrId::STR_STATE_SWIPE_UP}, "tapForReaderMenu",
                           StrId::STR_CAT_CONTROLS));
  if (!BoardConfig::hasTouch())
    emit(SettingInfo::Toggle(StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION, &CrossPointSettings::frontButtonFollowOrientation,
                             "frontButtonFollowOrientation", StrId::STR_CAT_CONTROLS));
  emit(SettingInfo::Enum(StrId::STR_LONG_PRESS_BEHAVIOR, &CrossPointSettings::longPressButtonBehavior,
                         {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                          StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION},
                         "longPressButtonBehavior", StrId::STR_CAT_CONTROLS));
  emit(buildLongPressMenuSetting());
  // Double-click power frontlight shortcut only exists on the X4 Pro.
  if (BoardConfig::isX4Pro())
    emit(SettingInfo::Toggle(StrId::STR_DBL_CLICK_PWR_LIGHT, &CrossPointSettings::doubleClickPwrLight,
                             "doubleClickPwrLight", StrId::STR_CAT_CONTROLS));
  emit(buildShortPowerButtonSetting());
  // Always persisted; getSettingsList() hides it when the IMU probe fails.
  emit(SettingInfo::Enum(StrId::STR_TILT_PAGE_TURN, &CrossPointSettings::tiltPageTurn,
                         {StrId::STR_STATE_OFF, StrId::STR_NORMAL, StrId::STR_INVERTED}, "tiltPageTurn",
                         StrId::STR_CAT_CONTROLS));
  emit(SettingInfo::Toggle(StrId::STR_PWR_BTN_FOOTNOTE_BACK, &CrossPointSettings::pwrBtnFootnoteBack,
                           "pwrBtnFootnoteBack", StrId::STR_CAT_CONTROLS));
  if (!BoardConfig::hasTouch())
    emit(SettingInfo::Toggle(StrId::STR_BACK_SHORT_TO_FILE_BROWSER, &CrossPointSettings::backShortToFileBrowser,
                             "backShortToFileBrowser", StrId::STR_CAT_CONTROLS));

  // Persisted here; set from the Connectivity -> Plugins popup.
  emit(SettingInfo::Toggle(StrId::STR_PLUGINS, &CrossPointSettings::pluginsEnabled, "pluginsEnabled").withWebHidden());
  emit(SettingInfo::Toggle(StrId::STR_PLUGIN_HUB, &CrossPointSettings::pluginHubPromptHidden, "pluginHubPromptHidden")
           .withWebHidden());

  // --- System ---
  emit(SettingInfo::Value(
      StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeoutMinutes,
      {CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1},
      "sleepTimeoutMinutes", StrId::STR_CAT_SYSTEM));
  emit(SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles, "showHiddenFiles",
                           StrId::STR_CAT_SYSTEM));
  emit(SettingInfo::Toggle(StrId::STR_LIBRARY_USE_METADATA, &CrossPointSettings::libraryUseMetadata,
                           "libraryUseMetadata", StrId::STR_CAT_SYSTEM));
  emit(SettingInfo::Toggle(StrId::STR_REMOVE_READ_FROM_RECENTS, &CrossPointSettings::removeReadBooksFromRecents,
                           "removeReadBooksFromRecents", StrId::STR_CAT_SYSTEM));
  emit(SettingInfo::Toggle(StrId::STR_MOVE_FINISHED_TO_READ, &CrossPointSettings::moveFinishedToReadFolder,
                           "moveFinishedToReadFolder", StrId::STR_CAT_SYSTEM));

  // OPDS download folder: persisted + web-exposed, but category-less so it
  // is hidden from the on-device Settings screen (edited via OPDS UI).
  emit(SettingInfo::String(StrId::STR_OPDS_DOWNLOAD_FOLDER, &SETTINGS.opdsDownloadFolder[0],
                           sizeof(SETTINGS.opdsDownloadFolder), "opdsDownloadFolder"));
  // OPDS download filename format: persisted + web-exposed, category-less so it
  // is hidden from the on-device Settings screen (cycled from the OPDS UI).
  emit(SettingInfo::Enum(StrId::STR_OPDS_FILENAME_FORMAT, &CrossPointSettings::opdsFilenameFormat,
                         {StrId::STR_FMT_AUTHOR_TITLE, StrId::STR_FMT_TITLE_AUTHOR, StrId::STR_FMT_TITLE},
                         "opdsFilenameFormat"));

  // Frontlight quick-panel state: persisted and web-exposed, but hidden
  // from the on-device Settings screen because the swipe panel owns it.
  emit(SettingInfo::Value(StrId::STR_BRIGHTNESS, &CrossPointSettings::frontlightBrightness, {0, 100, 5},
                          "frontlightBrightness"));
#if FREEINK_CAP_WARMLIGHT
  emit(SettingInfo::Value(StrId::STR_WARMTH, &CrossPointSettings::frontlightWarmth, {0, 100, 5}, "frontlightWarmth"));
#endif
  emit(SettingInfo::Toggle(StrId::STR_FRONTLIGHT, &CrossPointSettings::frontlightOn, "frontlightOn"));

  // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
  emit(SettingInfo::DynamicString(
      StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
      [](const std::string& v) {
        KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
        KOREADER_STORE.saveToFile();
      },
      "koUsername", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicString(
           StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
           [](const std::string& v) {
             KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
             KOREADER_STORE.saveToFile();
           },
           "koPassword", StrId::STR_KOREADER_SYNC)
           .withObfuscated());
  emit(SettingInfo::DynamicString(
      StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
      [](const std::string& v) {
        KOREADER_STORE.setServerUrl(v);
        KOREADER_STORE.saveToFile();
      },
      "koServerUrl", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicString(
      StrId::STR_KOREADER_CUSTOM_HEADER_1_NAME, [] { return KOREADER_STORE.getCustomHeaders()[0].name; },
      [](const std::string& v) {
        KOREADER_STORE.setCustomHeader(0, v, KOREADER_STORE.getCustomHeaders()[0].value);
        KOREADER_STORE.saveToFile();
      },
      "koHeader1Name", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicString(
           StrId::STR_KOREADER_CUSTOM_HEADER_1_VALUE, [] { return KOREADER_STORE.getCustomHeaders()[0].value; },
           [](const std::string& v) {
             KOREADER_STORE.setCustomHeader(0, KOREADER_STORE.getCustomHeaders()[0].name, v);
             KOREADER_STORE.saveToFile();
           },
           "koHeader1Value", StrId::STR_KOREADER_SYNC)
           .withObfuscated());
  emit(SettingInfo::DynamicString(
      StrId::STR_KOREADER_CUSTOM_HEADER_2_NAME, [] { return KOREADER_STORE.getCustomHeaders()[1].name; },
      [](const std::string& v) {
        KOREADER_STORE.setCustomHeader(1, v, KOREADER_STORE.getCustomHeaders()[1].value);
        KOREADER_STORE.saveToFile();
      },
      "koHeader2Name", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicString(
           StrId::STR_KOREADER_CUSTOM_HEADER_2_VALUE, [] { return KOREADER_STORE.getCustomHeaders()[1].value; },
           [](const std::string& v) {
             KOREADER_STORE.setCustomHeader(1, KOREADER_STORE.getCustomHeaders()[1].name, v);
             KOREADER_STORE.saveToFile();
           },
           "koHeader2Value", StrId::STR_KOREADER_SYNC)
           .withObfuscated());
  emit(SettingInfo::DynamicEnum(
      StrId::STR_SERVER_TYPE, {StrId::STR_CROSSPOINT, StrId::STR_KOSYNC_SERVER, StrId::STR_OTHER},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getServerType()); },
      [](uint8_t v) {
        KOREADER_STORE.setServerType(static_cast<KOReaderServerType>(v));
        KOREADER_STORE.saveToFile();
      },
      "koServerType", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicEnum(
      StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
      [](uint8_t v) {
        KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
        KOREADER_STORE.saveToFile();
      },
      "koMatchMethod", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicEnum(
      StrId::STR_SEND_METADATA, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getSendMetadata()); },
      [](uint8_t v) {
        KOREADER_STORE.setSendMetadata(v != 0);
        KOREADER_STORE.saveToFile();
      },
      "koSendMetadata", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicEnum(
      StrId::STR_SYNC_BEHAVIOR, {StrId::STR_ASK_EVERY_TIME, StrId::STR_SMART_SYNC},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getSyncBehavior()); },
      [](uint8_t v) {
        KOREADER_STORE.setSyncBehavior(static_cast<KOReaderSyncBehavior>(v));
        KOREADER_STORE.saveToFile();
      },
      "koSyncBehavior", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicEnum(
      StrId::STR_SYNC_CLIPPINGS, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getSyncClippings()); },
      [](uint8_t v) {
        if (KOREADER_STORE.getSyncClippings() == (v != 0)) return;
        KOREADER_STORE.setSyncClippings(v != 0);
        KOREADER_STORE.saveToFile();
      },
      "koSyncClippings", StrId::STR_KOREADER_SYNC));
  // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
  emit(SettingInfo::Toggle(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount,
                           "statusBarChapterPageCount", StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarBookProgressPercentage,
                           "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarProgressBar,
                         {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarProgressBar",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                         {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                         "statusBarProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                         {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarTitle",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Toggle(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery, "statusBarBattery",
                           StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_XTC_STATUS_BAR, &CrossPointSettings::xtcStatusBarMode,
                         {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP}, "xtcStatusBarMode",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  // Clock entries (persistence + web settings; the device UI is
  // ClockSettingsActivity under System settings).
  emit(SettingInfo::StaticEnum(StrId::STR_CLOCK, &CrossPointSettings::statusBarClock, STATUS_BAR_CLOCK_VALUES,
                               "statusBarClock", StrId::STR_CUSTOMISE_STATUS_BAR));
  // LEGACY: retired quarter-hour UTC offset (biased by 48). Persisted so
  // timezones::activeIndex() can migrate it into clockTimezone.
  emit(SettingInfo::Value(StrId::STR_CLOCK, &CrossPointSettings::clockUtcOffsetQ, {0, 104, 1}, "clockUtcOffsetQ",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat,
                         {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H}, "clockFormat",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  // Index into the append-only table in src/util/Timezones.cpp; 255 = unset.
  emit(SettingInfo::Value(StrId::STR_TIMEZONE, &CrossPointSettings::clockTimezone, {0, 255, 1}, "clockTimezone",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_CLOCK_DST, &CrossPointSettings::clockDst,
                         {StrId::STR_CLOCK_DST_AUTO, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, "clockDst",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Toggle(StrId::STR_CLOCK_IN_HEADER, &CrossPointSettings::clockShowInHeader, "clockShowHeader",
                           StrId::STR_CUSTOMISE_STATUS_BAR));
  // Persistence flag for NTP debounce. Resetting from the web UI forces a re-sync
  // on next WiFi connect, which is useful when crossing time zones.
  emit(SettingInfo::Toggle(StrId::STR_CLOCK_SYNCED, &CrossPointSettings::clockHasBeenSynced, "clockHasBeenSynced",
                           StrId::STR_CUSTOMISE_STATUS_BAR));
  if (BoardConfig::hasHomeKey()) {
    for (unsigned i = 0; i < 3; ++i) {
      emit(SettingInfo::StaticEnum(home_button::GESTURE_LABELS[i], home_button::FIELDS[i], home_button::ACTION_LABELS,
                                   home_button::KEYS[i], StrId::STR_CAT_CONTROLS));
    }
  }
}

std::vector<SettingInfo> getBaseSettingsList(const size_t extraCapacity) {
  // Count first so the one contiguous block is sized exactly.
  size_t count = 0;
  forEachBaseSetting([](void* context, SettingInfo&) { ++*static_cast<size_t*>(context); }, &count);
  std::vector<SettingInfo> v;
  v.reserve(count + extraCapacity);
  forEachBaseSetting(
      [](void* context, SettingInfo& info) {
        static_cast<std::vector<SettingInfo>*>(context)->push_back(std::move(info));
      },
      &v);
  return v;
}
