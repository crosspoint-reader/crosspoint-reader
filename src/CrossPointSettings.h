#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <cstdio>
#include <iosfwd>
#include <string>
#include <vector>

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
    QUICK_RESUME = 7,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_IMAGE_PICK_MODE { PICK_RANDOM = 0, PICK_SEQUENTIAL = 1, SLEEP_IMAGE_PICK_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    // Greyscale like NO_FILTER, but stretches the image's own tonal range first.
    ADAPTIVE_TONE = 3,
    // Greyscale too, but levels via the histogram's own CDF rather than its endpoints.
    // Reaches bimodal images that ADAPTIVE_TONE leaves flat, at the cost of more dither
    // texture in smooth gradients -- see AdaptiveTone.h.
    EQUALIZE_TONE = 4,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };
  enum QUICK_RESUME_SLEEP_SCREEN {
    QUICK_RESUME_NEVER = 0,
    QUICK_RESUME_AFTER_TIMEOUT = 1,
    QUICK_RESUME_SLEEP_SCREEN_COUNT
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
  enum STATUS_BAR_ITEMS_POSITION {
    STATUS_BAR_ITEMS_TOP = 0,
    STATUS_BAR_ITEMS_BOTTOM = 1,
    STATUS_BAR_ITEMS_POSITION_COUNT
  };

  // Which end of the status-item lane the clock sits on. There is deliberately no centre option:
  // the middle of the lane is the title's slot, so a centred clock would either displace the title
  // or collide with it.
  enum STATUS_BAR_CLOCK_POSITION {
    STATUS_BAR_CLOCK_LEFT = 0,
    STATUS_BAR_CLOCK_RIGHT = 1,
    STATUS_BAR_CLOCK_POSITION_COUNT
  };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
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
  enum FONT_FAMILY { BOOKERLY = 0, NOTOSANS = 1, FONT_FAMILY_COUNT };
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;
  // Font size options, in ASCENDING PIXEL ORDER, which is also the order the settings UI shows
  // them in -- a row's option index IS its stored value everywhere (device selector, web API,
  // settings JSON), so the two cannot be allowed to disagree.
  //
  // These values are PERSISTED and they were NOT always in this order. PT_10 was appended as 4
  // and sat between PT_18 and nothing, so the picker read "Small Medium Large X-Large
  // Tiny". That was tolerable while the labels were adjectives; it is plainly broken once they
  // read "12pt 14pt 16pt 18pt 10pt". Rather than add an index-to-value indirection for the sake
  // of one misplaced entry, the values are renumbered and old files are migrated on load -- see
  // FONT_SIZE_ORDER_VERSION and remapLegacyFontSize().
  //
  // Named by POINT SIZE, not by adjective. The adjectives were never accurate -- "medium" was
  // whatever 14 pt happened to be -- they ran out at "extra large" and degenerated into XX_LARGE,
  // and the labels a reader sees have been point sizes since the UI stopped guessing at names.
  // Only the VALUES are persisted, never the identifiers, so the rename cost nothing.
  //
  // PT_20/22/24/26 have no faces of their own: they render the 18 pt master scaled (see
  // BUILTIN_FACE_MAX_POINTS and GfxRenderer::insertScaledFont). In every other respect they are
  // ordinary rungs -- selectable as the default size, available as a per-book override, and
  // cached separately because each carries its own font ID.
  enum FONT_SIZE {
    PT_10 = 0,
    PT_12 = 1,
    PT_14 = 2,
    PT_16 = 3,
    PT_18 = 4,
    PT_20 = 5,
    PT_22 = 6,
    PT_24 = 7,
    PT_26 = 8,
    FONT_SIZE_COUNT
  };

  /// Bumped when the meaning of a stored reader size changes. A settings or recent-books file
  /// stamped lower holds values in an older meaning and is converted as it loads.
  ///
  /// 1 = PT_10 moved from 4 to 0 and everything below it shifted up one, so the enum runs in
  ///     pixel order.
  /// 2 = the values are point sizes, not FONT_SIZE values. An SD family offers the sizes it was
  ///     built at (7 pt, 13 pt), which no fixed enum can name.
  static constexpr uint8_t FONT_SIZE_ORDER_VERSION = 2;
  /// The first stamp whose values are point sizes. A fixed 2, not FONT_SIZE_ORDER_VERSION: a later
  /// bump must not turn every version-2 file back into enum values.
  static constexpr uint8_t FIRST_POINT_SIZE_VERSION = 2;

  /// Reader sizes are stored as POINT SIZES: fontPointSize, txtFontPointSize and a book's
  /// fontSizeOverride. FONT_SIZE survives only as the built-in ladder, i.e. which faces exist.
  static constexpr uint8_t DEFAULT_FONT_POINT_SIZE = 14;
  /// Bounds for a stored point size. A book's override is an int8_t with -1 for "follow the
  /// default", which caps it at 127. No .cpfont comes near that: the converter refuses a line
  /// height over 255 px, about 100 pt at 150 DPI. 1 is what SdCardFontRegistry::parseFilename()
  /// accepts.
  static constexpr uint8_t MIN_FONT_POINT_SIZE = 1;
  static constexpr uint8_t MAX_FONT_POINT_SIZE = 127;

  /// A persisted FONT_SIZE from a file stamped `fileVersion` (0 or 1), in the version-1 numbering.
  ///
  /// Out-of-range input is returned untouched rather than guessed at: it is either a
  /// hand-edited file or a value from a firmware newer than this one, and both are better left
  /// for the caller's own clamp to deal with.
  static constexpr uint8_t remapLegacyFontSize(const uint8_t stored, const uint8_t fileVersion) {
    if (fileVersion >= 1) return stored;
    // v0: PT_12=0 PT_14=1 PT_16=2 PT_18=3 PT_10=4. PT_20 did not exist.
    switch (stored) {
      case 0:
        return PT_12;
      case 1:
        return PT_14;
      case 2:
        return PT_16;
      case 3:
        return PT_18;
      case 4:
        return PT_10;
      default:
        return stored;
    }
  }

  /// A reader size read from a file stamped `fileVersion`, as a point size. Returns 0 when the
  /// stored value is unusable: absent (pass -1), out of range, or a FONT_SIZE no rung carries.
  ///
  /// The caller supplies the fallback because it differs: a setting falls back to
  /// DEFAULT_FONT_POINT_SIZE, a book's override to "follow the default".
  static constexpr uint8_t fontPointSizeFromStored(const int stored, const uint8_t fileVersion) {
    if (stored < 0 || stored > 255) return 0;
    const auto value = static_cast<uint8_t>(stored);
    if (fileVersion >= FIRST_POINT_SIZE_VERSION) {
      return value >= MIN_FONT_POINT_SIZE && value <= MAX_FONT_POINT_SIZE ? value : 0;
    }
    return fontSizePoints(remapLegacyFontSize(value, fileVersion));
  }

  /// The reader size ladder: every rung in ASCENDING PIXEL order, paired with the point size its
  /// faces are generated at.
  ///
  /// ONE table because there were four — this list, a copy in getTallerBuiltinReaderFontId(), the
  /// point-size map in SdCardFontSystem.cpp, and the pair of arrays in
  /// EpubReaderActivity::buildReaderFontSizeLadder(). Adding PT_20 meant updating all of them,
  /// and each failed differently and silently when missed: "one size bigger" would skip the new
  /// rung, SD fonts would load the wrong point size, the heading ladder would ignore it.
  ///
  /// FONT_SIZE values are frozen history: files stamped before FIRST_POINT_SIZE_VERSION stored them,
  /// and fontPointSizeFromStored() reads those files by these values. Never renumber or insert a
  /// rung in the middle -- an old file's "1" would then name a different size.
  ///
  /// Adding a size at the TOP is: append a rung here, add the case to getBuiltinReaderFontId(), and
  /// add it to SYNTH_FONT_SIZES in lib/EpdFont/scripts/font-sizes.sh for its font ID. main.cpp
  /// registers every rung above BUILTIN_FACE_MAX_POINTS as the master scaled, so it needs nothing.
  struct ReaderFontRung {
    FONT_SIZE size;
    uint8_t points;  ///< the point size it renders at
  };
  static constexpr ReaderFontRung FONT_SIZE_RUNGS[] = {
      {PT_10, 10}, {PT_12, 12}, {PT_14, 14}, {PT_16, 16}, {PT_18, 18},
      {PT_20, 20}, {PT_22, 22}, {PT_24, 24}, {PT_26, 26},
  };
  static constexpr int FONT_SIZE_RUNG_COUNT = static_cast<int>(sizeof(FONT_SIZE_RUNGS) / sizeof(FONT_SIZE_RUNGS[0]));

  /// The largest rung with faces of its own (font-sizes.sh's SYNTH_FONT_MASTER, which must agree).
  /// Every rung above it has none and draws this size's faces enlarged, at builtinFaceScale(), so
  /// its faces are the master. 20 pt had faces until they were dropped for the flash they took.
  static constexpr uint8_t BUILTIN_FACE_MAX_POINTS = 18;

  /// The scale a built-in rung draws its faces at: exactly 1 for a rung with faces of its own, its
  /// size over the master's above that -- 20 pt is the 18 pt faces at x1.11.
  static constexpr float builtinFaceScale(const uint8_t points) {
    return points > BUILTIN_FACE_MAX_POINTS ? static_cast<float>(points) / BUILTIN_FACE_MAX_POINTS : 1.0f;
  }

  /// The point size a FONT_SIZE renders at, or 0 if it names no rung.
  static constexpr uint8_t fontSizePoints(const uint8_t size) {
    for (const ReaderFontRung& r : FONT_SIZE_RUNGS) {
      if (r.size == size) return r.points;
    }
    return 0;
  }
  /// Every FONT_SIZE value must name exactly one rung. The settings UI indexes its label list by
  /// enum VALUE, so a value with no rung would render as a blank, selectable row.
  static constexpr bool ladderCoversEveryFontSize() {
    for (int v = 0; v < FONT_SIZE_COUNT; ++v) {
      if (fontSizePoints(static_cast<uint8_t>(v)) == 0) return false;
    }
    return true;
  }

  /// The built-in rung that draws `pt`: the closest one, ties to the smaller. Built-in faces exist
  /// only on the ladder, so a size chosen under an SD family is drawn with the nearest of them.
  /// Must agree with ReaderSizeList::builtin().snap(), which is what the settings row shows.
  static constexpr FONT_SIZE builtinRungForPoints(const uint8_t pt) {
    FONT_SIZE best = FONT_SIZE_RUNGS[0].size;
    int bestDiff = 256;
    for (const ReaderFontRung& r : FONT_SIZE_RUNGS) {
      const int diff = r.points > pt ? r.points - pt : pt - r.points;
      if (diff < bestDiff) {
        best = r.size;
        bestDiff = diff;
      }
    }
    return best;
  }

  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2, LINE_COMPRESSION_COUNT };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // UI Theme
  enum UI_THEME { CLASSIC = 0, LYRA = 1, LYRA_3_COVERS = 2, LYRA_CAROUSEL = 3 };

  // Size of the menu/chrome text. Each step rebinds the three logical UI font IDs one rung up
  // the Inter ladder (applyUiFontScale() in main.cpp) and adds the matching number of pixels to
  // every metric that has to hold a line of UI text (UiFontLadder::applyTo() in UiFontScale.h).
  //
  // Deliberately does NOT touch the reader's own font or its status bar: those feed the text
  // viewport, and changing the viewport invalidates every book's pagination cache. This setting
  // is about making menu rows easier to hit, not about re-laying-out books.
  enum UI_FONT_SIZE { UI_FONT_SIZE_DEFAULT = 0, UI_FONT_SIZE_LARGE = 1, UI_FONT_SIZE_COUNT };

  // Image rendering in EPUB reader
  enum IMAGE_RENDERING { IMAGES_DISPLAY = 0, IMAGES_PLACEHOLDER = 1, IMAGES_SUPPRESS = 2, IMAGE_RENDERING_COUNT };

  // File browser sort mode (per-session, not persisted)
  enum FILE_SORT_MODE { SORT_BY_NAME = 0, SORT_BY_DATE = 1, SORT_BY_SIZE = 2, SORT_BY_TYPE = 3, FILE_SORT_MODE_COUNT };

  // File browser sort direction (per-session, not persisted)
  enum FILE_SORT_DIRECTION { SORT_ASCENDING = 0, SORT_DESCENDING = 1, FILE_SORT_DIRECTION_COUNT };

  // How Browse Files shows a book: its filename; its title, author, series and progress; or its
  // cover in a grid.
  enum FILE_BROWSER_VIEW {
    BROWSER_VIEW_FILES = 0,
    BROWSER_VIEW_DETAILS = 1,
    BROWSER_VIEW_COVERS = 2,
    FILE_BROWSER_VIEW_COUNT
  };

  // Action mapped to each tilt gesture direction.
  enum TILT_GESTURE_ACTION {
    TILT_ACT_NONE = 0,
    TILT_ACT_NEXT_PAGE = 1,
    TILT_ACT_PREV_PAGE = 2,
    TILT_GESTURE_ACTION_COUNT
  };

  // Whether touch drives the non-reader screens at all. See touchUiControls.
  enum TOUCH_UI_CONTROLS { TOUCH_UI_OFF = 0, TOUCH_UI_ON = 1, TOUCH_UI_CONTROLS_COUNT };

  // Whether a list-row tap activates immediately or first moves selection.
  enum TOUCH_LIST_ACTIVATION {
    TOUCH_LIST_SELECT_THEN_ACTIVATE = 0,
    TOUCH_LIST_ACTIVATE_IMMEDIATELY = 1,
    TOUCH_LIST_ACTIVATION_COUNT
  };

  // How the reading surface responds to touch. Off leaves the page inert, so a
  // thumb resting on the panel can never turn a page (see
  // docs/contributing/touch-architecture.md, "Gating").
  enum TOUCH_READER_CONTROLS {
    TOUCH_READER_OFF = 0,
    TOUCH_READER_ON = 1,            // tap the outer thirds: left = back, right = forward
    TOUCH_READER_SWIPE = 2,         // horizontal swipes turn pages; taps stay free
    TOUCH_READER_INVERTED_TAP = 3,  // mirrored tap zones
    TOUCH_READER_CONTROLS_COUNT
  };

  // Text darkness for AA glyph rendering (forwarded to GfxRenderer::setTextDarkness)
  enum TEXT_DARKNESS {
    DARKNESS_NORMAL = 0,      // true 4-level AA
    DARKNESS_DARK = 1,        // historical default — bolder dark-gray AA
    DARKNESS_EXTRA_DARK = 2,  // both AA shades pushed to darkest gray state
    DARKNESS_MAXIMUM = 3,     // skip grayscale passes — AA pixels stay solid black from BW pass
    // Lighter than NORMAL: both AA shades take the LIGHT tone instead of their
    // own. Appended rather than placed at 0, where it belongs on a darkness
    // scale, because this value is PERSISTED — inserting would silently turn
    // every saved DARKNESS_DARK into DARKNESS_NORMAL and so on down the list.
    // The menu therefore reads Normal, Dark, Extra Dark, Maximum, Lighter.
    DARKNESS_LIGHT = 4,
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

  // RETIRED. Kept only so the value this firmware has been persisting under "timeZone" can be
  // read back and migrated into clockTimezone, which indexes the table in util/Timezones.cpp.
  // Nothing selects a timezone through these any more -- see LEGACY_TO_TABLE, which records
  // where each one lands. Do not add to this enum; add to the table.
  enum LEGACY_TIMEZONE {
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
    TZ_LEGACY_COUNT = 19
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
  // Quick Resume on Timeout: keep current page on display with a moon icon when sleeping by timeout,
  // and on wake restore the page directly (skipping the boot screen).
  uint8_t quickResumeSleepScreen = QUICK_RESUME_NEVER;
  // Status bar settings
  uint8_t statusBarChapterPageCount = 1;
  // Printed ("physical") page number from the book's page-list. Drawn in parentheses to the left of
  // the device page counter when both share a location; otherwise on its own. Default on.
  uint8_t statusBarPrintedPage = 1;
  uint8_t statusBarBookProgressPercentage = 1;
  uint8_t statusBarUpperProgressBar = HIDE_PROGRESS;
  uint8_t statusBarUpperProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarLowerProgressBar = HIDE_PROGRESS;
  uint8_t statusBarLowerProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarItemsPosition = STATUS_BAR_ITEMS_BOTTOM;
  uint8_t statusBarTitle = CHAPTER_TITLE;
  uint8_t statusBarBattery = 1;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  uint8_t textAntiAliasing = 1;
  // X3-only: when on, the AA refresh uses the 7-frame community grayscale LUT
  // (~130 ms panel time) instead of the OEM 53-frame LUT (~2.4 s). Mid-tones
  // run slightly darker than X4. Matches what papyrix-reader has shipped since
  // 2025-11. Default on — the 2.2 s/page win dwarfs the subtle mid-tone shift.
  // No effect on X4.
  //
  // JSON key was bumped from "fastAntiAliasing" to "fastAntiAliasingV2" when
  // the default flipped to 1: existing settings files with the old key are
  // ignored, so every device picks up the new C++ default on next load.
  uint8_t fastAntiAliasing = 1;
  // Text darkness (0 = normal, 1 = dark, 2 = extra dark). Default 1 preserves
  // historical AA rendering (both grayscale shades drawn in the MSB pass).
  uint8_t textDarkness = DARKNESS_DARK;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Front button remap (logical -> hardware)
  // Used by MappedInputManager to translate logical buttons into physical front buttons.
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // Reader font settings (EPUB)
  uint8_t fontFamily = BOOKERLY;
  // SD card font family name (empty = use built-in fontFamily)
  char sdFontFamilyName[32] = "";
  // Folder name under /dictionaries (or /.dictionaries) of the StarDict
  // dictionary used for word lookup; empty means no dictionary is selected.
  // A folder NAME, not a path: DictionaryRegistry::resolveBasePath rejects
  // separators and dot prefixes so a hand-edited value cannot escape the roots.
  char dictionaryName[32] = "";
  // Point size of the EPUB reader font, persisted as "fontSize". Shown and drawn snapped to the
  // sizes the active family offers (ReaderSizeList), never rewritten to them.
  uint8_t fontPointSize = DEFAULT_FONT_POINT_SIZE;
  // Reader font settings (TXT / MD) — defaults to EPUB settings when not explicitly set
  uint8_t txtFontFamily = NOTOSANS;
  char txtSdFontFamilyName[32] = "";
  uint8_t txtFontPointSize = DEFAULT_FONT_POINT_SIZE;  // persisted as "txtFontSize"
  uint8_t lineSpacing = NORMAL;
  uint8_t paragraphAlignment = JUSTIFIED;
  // Auto-sleep timeout in minutes (0 = never sleep, 1-60).
  uint8_t sleepTimeoutMinutes = 10;
  // Full-refresh frequency in pages (0 = never full-refresh, 1-60).
  uint8_t refreshFrequencyPages = 15;
  // Perform a half refresh on the page immediately following an EPUB page that displayed images.
  uint8_t halfRefreshAfterImagePage = 1;
  uint8_t hyphenationEnabled = 0;
  // Snap near-body font sizes (within ±10% of the body size) to plain body text, so publisher
  // sizing renders native — both inline wrappers like <span style="font-size:0.92em"> around whole
  // paragraphs and a size stated on the block itself (p.body { font-size: 1.1em }). Default on.
  uint8_t fontSizeNormalization = 1;

  // Reader screen margin settings
  uint8_t screenMargin = 5;

  // Extra clearance from the panel edge, on TOP of the board profile's own viewableInsets --
  // see GfxRenderer::setViewablePaddingProvider. The profile says where the case covers the
  // glass; this says how much further in the reader wants content to sit, because on some
  // boards (the T5 S3) the plastic cover comes near enough to the live pixels that text at the
  // profile inset is hard to read.
  //
  // 0 = Narrow, i.e. exactly what the board declares and what every device did before this
  // existed, so the default changes nothing anywhere.
  enum EDGE_MARGIN : uint8_t { EDGE_MARGIN_NARROW = 0, EDGE_MARGIN_MEDIUM, EDGE_MARGIN_LARGE };
  uint8_t edgeMargin = EDGE_MARGIN_NARROW;
  static constexpr int EDGE_MARGIN_STEP_PX = 5;
  // Pixels to inset each edge by. Clamped against a corrupt settings file rather than trusted:
  // this reaches a renderer that has no other opinion about it.
  int getEdgeMarginPadding() const {
    const uint8_t step = edgeMargin <= EDGE_MARGIN_LARGE ? edgeMargin : EDGE_MARGIN_NARROW;
    return step * EDGE_MARGIN_STEP_PX;
  }
  // OPDS download destination folder ("" = SD root). Global; edited from the
  // OPDS server list. Persisted via a category-less SettingInfo::String in
  // SettingsList.h, so it stays out of the on-device Settings screen.
  char opdsDownloadFolder[64] = "";
  // On-disk filename format for OPDS downloads (0=Author-Title default, 1=Title-Author,
  // 2=Title). See OpdsFilenameFormat. Persisted via a category-less SettingInfo::Enum,
  // edited from the OPDS server list; hidden from the on-device Settings screen.
  uint8_t opdsFilenameFormat = 0;
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // UI Theme
  uint8_t uiTheme = LYRA;
  // Acknowledge a press that starts a slow screen change. See ActivityManager::showBusyIndicator().
  uint8_t showBusyIndicator = 1;
  // NOT a preference: the measured cost of a FAST refresh on this panel, carried across boots so
  // the first decision after a reboot is as good as the last one before it. Written from
  // HalDisplay's measurement, never from the UI, and absent from the JSON settings file for that
  // reason. 0 means not yet measured.
  uint16_t measuredFastRefreshMs = 0;
  // Menu/chrome text size (UI_FONT_SIZE)
  uint8_t uiFontSize = UI_FONT_SIZE_DEFAULT;
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // --- Frontlight / backlight (boards with FrontlightConfig; T5S3, X4 Pro) ---
  // Brightness is a 0-100 percentage the SDK maps through a perceptual gamma
  // curve. The floor the UI offers is HalFrontlight::MIN_BRIGHTNESS, not 0:
  // "off" is frontlightOn, not a zero level.
  uint8_t frontlightBrightness = 60;
  // Warm/cool mix, 0 = fully cool .. 100 = fully warm. Only meaningful on a
  // two-channel board (X4 Pro); inert on the T5S3's single backlight channel.
  uint8_t frontlightWarmth = 50;
  uint8_t frontlightOn = 0;
  // Whether waking restores the light. Off by default so a device that wakes in
  // a dark bag does not light up; brightness/warmth are always restored.
  uint8_t frontlightRestoreOnWake = 1;
  // Use book's embedded CSS styles for EPUB rendering (1 = enabled, 0 = disabled)
  uint8_t embeddedStyle = 1;
  // Publisher Fonts: 0 = off, 1 = a book with an embedded TrueType body font is drawn in it (BookFonts). Off by
  // default: opening such a book installs its family into the single glyph store, and so does the next book.
  uint8_t publisherFonts = 0;
  // Show hidden files/directories (starting with '.') in the file browser (0 = hidden, 1 = show)
  uint8_t showHiddenFiles = 0;
  // Show file extensions in the file browser (0 = hidden, 1 = show)
  uint8_t showFileExtensions = 0;
  // Browse Files view (FILE_BROWSER_VIEW). Not All Files or the pickers, which list files.
  uint8_t fileBrowserView = BROWSER_VIEW_FILES;
  // Image rendering mode in EPUB reader
  uint8_t imageRendering = IMAGES_DISPLAY;
  // Show a placeholder for large images (>800×600 source pixels) instead of decoding immediately.
  // The user can press OK on the placeholder page to decode the image on demand.
  uint8_t largeImagePlaceholder = 1;
  // Tilt-based page turning (X3 only — requires QMI8658 IMU)
  uint8_t tiltPageTurn = 0;
  // Action when the computed tilt value crosses the positive threshold.
  uint8_t tiltPositiveAction = TILT_ACT_NEXT_PAGE;
  // Action when the computed tilt value crosses the negative threshold.
  uint8_t tiltNegativeAction = TILT_ACT_PREV_PAGE;
  // Touch page turning on the reading surface (touch boards only). Defaults to
  // tap zones, which is what upstream ships; the X4 Pro has no back/confirm
  // button, so leaving it off would strand the reader.
  uint8_t touchReaderControls = TOUCH_READER_ON;
  // Whether touch does anything OUTSIDE the reader: tapping list rows, book
  // covers, the home menu, the on-screen button-hint strip, and the back/home
  // edge gestures. Off leaves those screens button-only, for a device carried in
  // a bag or read one-handed where a stray brush kept selecting things.
  //
  // The reader is deliberately not covered — it has touchReaderControls, and one
  // behaviour with two switches is a support question waiting to happen. On a
  // board whose Back/Confirm come from the capacitive Home key those still work
  // with this off: HalGPIO synthesises them as BUTTON edges below this layer, so
  // turning touch navigation off cannot strand anyone in a menu.
  uint8_t touchUiControls = TOUCH_UI_ON;
  uint8_t touchListActivation = TOUCH_LIST_SELECT_THEN_ACTIVATE;

  // Bumped whenever the SHIPPED gesture defaults change. A stored value below
  // this makes the loader ignore the file's gesture keys and keep the compiled
  // defaults instead.
  //
  // It exists because BTN_DEFAULT is 0, which makes "the user chose Built-in"
  // and "this key was written before the default existed" the same byte on disk.
  // Without the stamp a changed default reaches only devices that have never
  // saved their settings — i.e. none — and the symptom on hardware is a gesture
  // that silently does nothing, which looks exactly like a firmware bug. Two
  // device sessions were spent on that.
  //
  // A bump DISCARDS gesture customisation, deliberately: while the defaults are
  // still being tuned on hardware that is the useful trade. Once they settle,
  // stop bumping it and the whole mechanism goes inert.
  // 2: BTN_CYCLE_ORIENTATION_BACK was inserted before the board-gated light
  //    block, shifting BTN_LIGHT_* up by one. Stored gesture values naming a
  //    light action would otherwise be read as the action next to it.
  // 3: the vertical swipes changed MEANING, from "which half of the screen did
  //    this start in" to "which edge column, excluding the top/bottom bands".
  //    A stored value was chosen against a gesture that no longer exists, so
  //    the compiled defaults have to win. The same bump also covers the light
  //    toggle moving from gestLongTapTop to gestLongTapTopLeft, and the new
  //    outward page-turn swipes — all of which shipped together, so one stamp
  //    is enough for the lot.
  static constexpr uint8_t GESTURE_DEFAULTS_VERSION = 3;
  uint8_t gestureDefaultsVersion = GESTURE_DEFAULTS_VERSION;

  // --- Gesture actions (touch boards) ---------------------------------------
  // One BUTTON_ACTION per gesture, exactly like the per-button short/double/long
  // fields above, and with the same meaning for BTN_DEFAULT: leave this input
  // alone and let the reader interpret it as it always has. That is why every
  // one of these defaults to BTN_DEFAULT rather than to the behaviour it would
  // replace — an unconfigured device must behave as though gestures were never
  // added. See src/TouchGestures.h for the zone geometry and the field table.
  //
  // Only consulted inside the reader. Everywhere else the screen's own touch
  // targets (list rows, the home grid, the button-hint strip) own the contact.
  // Horizontal swipes stay Built-in: in Swipe mode they ARE the page turn, and
  // binding them would override that in every mode.
  uint8_t gestSwipeLeft = BTN_DEFAULT;
  uint8_t gestSwipeRight = BTN_DEFAULT;
  // Vertical swipes are anchored to the left/right EDGE COLUMN they start in, which is
  // where KOReader puts the same controls (DSWIPE_ZONE_LEFT_EDGE adjusts the frontlight,
  // RIGHT_EDGE its warmth) and where a thumb already rests. The columns exclude the top
  // and bottom bands, so they never contend with the light panel or the reader menu —
  // TapZones::edgeColumnFor() carries that rule and the reason for it.
  //
  // The left column is brightness and the right warmth, which means neither direction can
  // reach "off": dimming clamps at MIN_BRIGHTNESS, and brightening an unlit panel lights it
  // (see BTN_LIGHT_BRIGHTER in main.cpp). Turning the light OFF therefore stays on
  // gestLongTapTop below, which is where it already was — a long press being deliberate
  // enough not to fire while repositioning a grip.
  //
  // Warmth is dropped to Built-in by dropUnsupportedActions() on a board with a
  // single-channel light, so the right column is simply free there.
  uint8_t gestSwipeUpLeft = BTN_LIGHT_BRIGHTER;
  uint8_t gestSwipeUpRight = BTN_LIGHT_WARMER;
  uint8_t gestSwipeDownLeft = BTN_LIGHT_DIMMER;
  uint8_t gestSwipeDownRight = BTN_LIGHT_COOLER;
  // Outward horizontal swipes: ten pages back in the back zone, ten forward in the
  // forward zone. Safe to bind by default where the tap zones are not, because these
  // route through dispatchButtonAction(BTN_PAGE_*_10) exactly as the physical buttons
  // do -- there is no separate end-of-book flow for a ten-page jump to bypass, which
  // is what keeps every tap zone on Built-in. Magnitude follows KOReader's
  // double_tap_left_side / double_tap_right_side.
  //
  // In Swipe reading mode these WIN over the plain page-turn swipe inside the outer
  // thirds, since GestureEventManager resolves first. That is the trade: the outer
  // thirds lose the one-page swipe and keep the one-page tap.
  uint8_t gestSwipeInLeftZone = BTN_PAGE_BACK_10;
  uint8_t gestSwipeInRightZone = BTN_PAGE_FORWARD_10;
  // Every tap zone stays Built-in, and must. The reader's own tap handling is
  // what implements Touch Reading Controls (Off / Tap / Swipe / Inverted tap)
  // and the end-of-book flow; binding a zone to BTN_PAGE_BACK would look
  // identical on a normal page and quietly bypass both. "Tap left = previous
  // page" is already true through that path.
  uint8_t gestTapLeft = BTN_DEFAULT;
  uint8_t gestTapRight = BTN_DEFAULT;
  uint8_t gestTapCentre = BTN_DEFAULT;
  uint8_t gestTapTop = BTN_DEFAULT;
  uint8_t gestTapBottom = BTN_DEFAULT;
  // Long taps were dead before gestures existed, so these take nothing away.
  // Chapter skip on a long press and lookup-on-hold follow CrossInk.
  uint8_t gestLongTapLeft = BTN_PREV_SECTION;
  uint8_t gestLongTapRight = BTN_NEXT_SECTION;
  uint8_t gestLongTapCentre = BTN_DICTIONARY;
  // The light toggle moved off this row onto the top-left CORNER below. A full-width
  // band across the top of the page is easy to catch when shifting grip, and toggling
  // the light by accident mid-paragraph is a whole-screen event; a corner is a place
  // you have to mean. KOReader puts its frontlight toggle in a corner for the same
  // reason.
  uint8_t gestLongTapTop = BTN_DEFAULT;
  uint8_t gestLongTapBottom = BTN_STAR_PAGE;
  // Corners, long press only, and live on EVERY screen rather than only in the reader --
  // see GestureEventManager's scope note. Only the top-left ships bound: it is the light
  // on/off that the edge-column brightness swipes cannot reach, since dimming clamps at
  // MIN_BRIGHTNESS.
  //
  // One gesture for one meaning everywhere, deliberately. The alternative considered was a
  // separate home-screen gesture, and it was rejected: a reader should not have to know
  // which screen they are on to know how to turn the light off. The light submenu on the
  // top-edge down-swipe remains the discoverable route, and double-press Power the one that
  // needs no touch at all.
  //
  // The other three are offered rather than assigned, following KOReader, which ships
  // every corner HOLD as nil.
  uint8_t gestLongTapTopLeft = BTN_LIGHT_TOGGLE;
  uint8_t gestLongTapTopRight = BTN_DEFAULT;
  uint8_t gestLongTapBottomLeft = BTN_DEFAULT;
  uint8_t gestLongTapBottomRight = BTN_DEFAULT;
  // Pinch resizes text and a two-finger turn turns the page: the two gestures
  // whose meaning users already carry with them from every other device.
  uint8_t gestPinchIn = BTN_FONT_SIZE_SMALLER;
  uint8_t gestPinchOut = BTN_FONT_SIZE_LARGER;
  // Opposite directions, so turning two fingers back the way they came undoes
  // the rotation instead of advancing three more steps. Which visual direction
  // "forward" is depends on the ORIENTATION enum's order, which is not obvious
  // from its names — if it reads inverted on hardware, swap these two lines.
  uint8_t gestRotateCw = BTN_CYCLE_ORIENTATION;
  uint8_t gestRotateCcw = BTN_CYCLE_ORIENTATION_BACK;

  // Enable synthetic TOC fallback for malformed/sparse TOC books (1 = enabled, 0 = disabled)
  uint8_t syntheticTocFallback = 1;
  // Default bionic reading in EPUB pages when no per-book override is set (1 = enabled, 0 = disabled)
  uint8_t bionicReading = 0;
  // Guide dots reading aid in EPUB pages: a small dot centered in each inter-word gap
  // (1 = enabled, 0 = disabled). Idea from CrossInk (https://github.com/uxjulia/CrossInk).
  uint8_t guideDots = 0;
  // Expand semantic EPUB footnote references with a short inline preview.
  uint8_t inlineFootnotePreviews = 0;
  // Automatically push reading progress to the KOReader sync server when leaving the reader
  // (1 = enabled, 0 = disabled). The push only fires when credentials are configured and the
  // reader session advanced at least koSyncMinSessionPages pages, and is skipped when remote
  // progress is already ahead.
  uint8_t koSyncOnBookClose = 0;
  // Pages that must be turned in a reader session before auto-push-on-close will fire. Opening a
  // book to check the cover or skim the TOC should not burn a network round-trip, but how much
  // reading counts as "a session" is a matter of taste — someone reading a page or two over
  // breakfast wants a lower bar than someone who only reads in long sittings.
  uint8_t koSyncMinSessionPages = 3;
  // Move finished book to /COMPLETED when the end-of-book screen action is selected.
  uint8_t moveFinishedBooksToCompleted = 0;
  // Remove finished book from Recent Books when the end-of-book screen action is selected.
  uint8_t removeFinishedBooksFromRecents = 0;
  // Push reading progress to the KOReader sync server when the end-of-book screen action is
  // selected (1 = enabled, 0 = disabled). Only offered when KOReader credentials are configured;
  // composes with whichever action (home / open next / OPDS search) was picked rather than
  // replacing it — see BookFinished::launchFinishedBookFlow.
  uint8_t syncFinishedBookToKOReader = 0;
  // Keyboard layouts the language key cycles through: bit i enables keyboard_layouts::ALL[i]
  // (see activities/util/KeyboardLayoutSet.h). 0 means "not configured" -- the UI language's
  // layout plus English -- so a reader who never opens the screen follows the UI language.
  uint16_t keyboardLayouts = 0;
  // Show clock in the reader status bar
  uint8_t statusBarClock = 0;
  // Which end of the status-item lane the clock is drawn at (see STATUS_BAR_CLOCK_POSITION).
  uint8_t statusBarClockPosition = STATUS_BAR_CLOCK_LEFT;
  // Clock format: 0 = 24h (14:00), 1 = 12h (2:00pm)
  uint8_t clockFormat12h = 0;
  // LEGACY timezone selection, a LEGACY_TIMEZONE value. Still READ from the settings file -- it
  // is the only record of what a device chose before clockTimezone existed, and
  // timezones::activeIndex() migrates it -- but no longer written: it left the settings list, so
  // the key drops out of the file on the first resave after the migration. See the block in
  // JsonSettingsIO::loadSettings that reads it.
  uint8_t timeZone = TZ_UTC;
  // Timezone selection: an index into the table in util/Timezones.cpp. 255 means "never chosen
  // on this firmware", which is what sends activeIndex() to the legacy value above. Deliberately
  // NOT 0 -- index 0 is a real zone (Midway), so a zero default would silently move every
  // unmigrated device there.
  uint8_t clockTimezone = 255;
  // Preferred NTP server (host or IP). Empty = use built-in servers only
  // (Cloudflare anycast IP + pool.ntp.org). When set, it is polled first, with
  // the built-ins kept as fallbacks. Passed into HalClock::syncNtp() by callers.
  char ntpServer[64] = "";
  // Use clock and keep the LP timer running during deep sleep (GPIO13 HIGH)
  // so time can be accurately restored on wake. Increases sleep current by ~3-4 mA.
  uint8_t useClock = 0;
  // Weather on or off as a feature. Off removes it from the home screen and from "More" alike;
  // which of the two it sits on is showWeatherOnHome.
  uint8_t useWeather = 1;
  // Which entries sit on the home screen (1) rather than behind its "More" entry (0). An entry
  // with nothing behind it (no bookmarks, weather off) shows in neither place.
  // Settings has no switch: it is where these are turned back on. See HomeMenu.cpp.
  uint8_t showLibraryOnHome = 1;
  // On by default, because a screen nobody can find is a screen nobody reads.
  uint8_t showReadingStatsOnHome = 1;
  uint8_t showBookmarksOnHome = 1;
  uint8_t showFileTransferOnHome = 1;
  uint8_t showWeatherOnHome = 1;
  // Include release candidate builds when checking for OTA updates.
  uint8_t includeBetaUpdates = 0;
  // Accept any TLS certificate on https requests (1 = skip validation).
  //
  // For self-hosted servers with a private CA or a self-signed certificate —
  // an OPDS catalog or a local KOReader sync server — where the alternative is
  // that the device simply cannot reach them. It disables authentication of the
  // peer for every https call the firmware makes, so credentials sent to such a
  // server are only as safe as the network path. OTA is deliberately exempt:
  // firmware is executed, so it stays verified whatever this is set to.
  uint8_t skipHttpsValidation = 0;

  // Configurable actions for short / double / long press on each logical button.
  //
  // BTN_DEFAULT means "do the activity's natural behaviour for this button" — it
  // is context-sensitive on purpose (e.g. Confirm-short opens the reader menu but
  // selects an entry in a list), which is why it can't be replaced by baking a
  // fixed action into the defaults. BTN_IGNORE is the opposite: explicitly do
  // nothing and suppress the built-in behaviour for that press.
  //
  // New values MUST be appended before BUTTON_ACTION_COUNT — the settings option
  // list (SettingsList.h btnActionOptions) and persisted JSON map by numeric value.
  enum BUTTON_ACTION {
    BTN_DEFAULT = 0,
    BTN_PAGE_FORWARD,
    BTN_PAGE_BACK,
    BTN_PAGE_FORWARD_10,
    BTN_PAGE_BACK_10,
    BTN_GO_HOME,
    BTN_SLEEP,
    BTN_FORCE_REFRESH,
    BTN_FORCE_FAST_REFRESH,
    BTN_OPEN_TOC,
    BTN_OPEN_BOOKMARKS,
    BTN_STAR_PAGE,
    BTN_FOOTNOTES,
    BTN_NEXT_SECTION,
    BTN_PREV_SECTION,
    BTN_EXIT_READER,
    BTN_READER_MENU,
    BTN_TOGGLE_BIONIC_READING,
    BTN_KOREADER_SYNC,
    BTN_CYCLE_FONT_SIZE,
    BTN_CYCLE_ORIENTATION,
    BTN_QUICK_OVERRIDES,
    BTN_IGNORE,
    BTN_DICTIONARY,
    // Directional font sizing. BTN_CYCLE_FONT_SIZE wraps in one direction, which
    // is fine for a button but useless for a pinch: pinching in must make the
    // text smaller and pinching out bigger, or the gesture means nothing.
    BTN_FONT_SIZE_SMALLER,
    BTN_FONT_SIZE_LARGER,
    // Flip touchUiControls. Worth a shortcut rather than a settings-screen trip
    // only: the reason to silence touch is usually momentary (reading in bed,
    // a book in a bag), and the reader is the one place it stays live, so a
    // gesture bound to this can always turn it back on.
    BTN_TOGGLE_TOUCH_UI,
    // The reverse of BTN_CYCLE_ORIENTATION. Same reasoning as the directional
    // font sizes: a single cycling action is fine on a button, but the two
    // rotation gestures are a PAIR — turning two fingers one way and then back
    // must undo, not advance three more steps.
    BTN_CYCLE_ORIENTATION_BACK,
    // --- Board-gated actions MUST stay at the end of this enum ---------------
    // SettingsList builds each button's option list positionally: index i in the
    // list is action value i. Actions it drops on boards that cannot perform them
    // therefore have to be the LAST entries, or dropping them renumbers every
    // action after them and a settings file written on one board is misread on
    // another. Add unconditional actions above this line.
    //
    // Frontlight/backlight control. Global rather than reader-scoped: the light
    // is as wanted on the home screen as it is mid-page.
    //
    // TWO gated blocks, in NARROWING order — the warm pair needs a second LED
    // channel that most lit boards do not have, so it is a strict subset of the
    // block above and must come after it. Dropping the narrower block on a
    // single-channel board then leaves the wider one's values untouched, which
    // is the whole reason the gated actions sit at the end.
    BTN_LIGHT_TOGGLE,
    BTN_LIGHT_BRIGHTER,
    BTN_LIGHT_DIMMER,
    // Colour temperature, on a warm/cool board only (X4 Pro). Brightness is the
    // TOTAL light and warmth splits it between the two strings, so these shift
    // the mix without changing how bright the page looks.
    BTN_LIGHT_WARMER,
    BTN_LIGHT_COOLER,
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
  uint8_t btnDoubleLeft = BTN_PAGE_BACK_10;
  uint8_t btnDoubleRight = BTN_PAGE_FORWARD_10;
  uint8_t btnDoublePageBack = BTN_DEFAULT;
  uint8_t btnDoublePageForward = BTN_DEFAULT;
  // The reading light, on the one control no overlay can ever cover and every board has.
  // It works from every screen and needs no digitiser at all, which makes it the fallback
  // for the top-left corner hold on a board where touch is off or awkward.
  //
  // Safe as a DOUBLE press specifically: a single press still sleeps, so nothing about the
  // power button's primary job changes. dropUnsupportedActions() clears this back to
  // Built-in on a board with no light, so X3/X4 are unaffected.
  uint8_t btnDoublePower = BTN_LIGHT_TOGGLE;

  // Long-press actions (default: built-in)
  uint8_t btnLongBack = BTN_DEFAULT;
  uint8_t btnLongConfirm = BTN_DEFAULT;
  uint8_t btnLongLeft = BTN_PREV_SECTION;
  uint8_t btnLongRight = BTN_NEXT_SECTION;
  uint8_t btnLongPageBack = BTN_DEFAULT;
  uint8_t btnLongPageForward = BTN_DEFAULT;
  uint8_t btnLongPower = BTN_DEFAULT;

  ~CrossPointSettings() = default;

  // Get singleton instance
  static CrossPointSettings& getInstance() { return instance; }

  // How long the power button must be held to put the device to sleep, measured from the
  // press edge in loop(). Device-measured at 401-410 ms of real hold on X3 and X4.
  static constexpr uint16_t getPowerButtonDuration() { return 400; }

  // The same gesture in the other direction, but the wake gate compares against millis()
  // — time since app start, not since the press (see HalGPIO::verifyPowerButtonWakeup).
  // The press that caused the wake began before the app did, so the ROM and second-stage
  // bootloader are already part of the user's hold and nothing charges for them: the real
  // hold is prelude + this value. Shortened from the sleep threshold to claw some of that
  // asymmetry back, and deliberately not shortened further — the gate's job is to reject a
  // stray pocket press, and the prelude is the only margin left doing that.
  static constexpr uint16_t getPowerWakeHoldDuration() { return 300; }
  int getReaderFontId() const;
  int getTxtReaderFontId() const;
  // Pure built-in lookup (family + ladder rung -> font ID), independent of SD-card font selection.
  // Takes a FONT_SIZE rather than a uint8_t so a stored point size cannot be passed by mistake;
  // those go through getBuiltinReaderFontIdForPoints().
  static int getBuiltinReaderFontId(uint8_t family, FONT_SIZE size);
  // The built-in face that draws a stored point size: the nearest rung (builtinRungForPoints()).
  // Used by the per-book fontFamilyOverride path, so an override forces back to a known built-in
  // even when an SD font is the global default.
  static int getBuiltinReaderFontIdForPoints(const uint8_t family, const uint8_t pt) {
    return getBuiltinReaderFontId(family, builtinRungForPoints(pt));
  }
  // Heading sizing: return the built-in fontId `stepUp` sizes taller than `size` for
  // `family`, clamped at the largest size. Steps walk the ascending-pixel ladder
  // (PT_10<PT_12<PT_14<PT_16<PT_18), not the FONT_SIZE enum order. `actualStep`
  // (out) receives how many steps were actually taken before the cap (so the caller can
  // compute a residual multiplier when clamped). Returns 0 for unknown families.
  static int getTallerBuiltinReaderFontId(uint8_t family, uint8_t size, uint8_t stepUp, uint8_t* actualStep = nullptr);

  bool saveToFile() const;
  bool loadFromFile();
  void loadStartupFromNvs();
  void saveStartupToNvs() const;

  static void validateFrontButtonMapping(CrossPointSettings& settings);

  // Reset every button and gesture mapping that names an action this board
  // cannot perform back to BTN_DEFAULT.
  //
  // Two things need it. The board-gated actions are dropped from the settings
  // screen's option list, so a stored value naming one would render blank and
  // could not be edited away; and an SD card moved from a board with a
  // frontlight to one without carries exactly such values. The capabilities are
  // passed in rather than queried so this stays free of the HAL and testable on
  // the host. hasWarmLight is the narrower of the two: a board can have a light
  // and no second warm channel, and then only the warmth pair goes.
  static void dropUnsupportedActions(CrossPointSettings& settings, bool hasFrontlight, bool hasWarmLight);

  // True for an action only the reader can carry out. Such an action must NOT be
  // consumed on another screen: doing so would swallow the input and shadow that
  // screen's own handling of it. Buttons let the event fall through to the
  // activity; gestures decline to claim the contact at all.
  //
  // Everything not named here is global — go home, sleep, the refreshes,
  // bookmarks, the reading light, the touch-navigation switch — and works
  // wherever it is triggered.
  static bool isReaderScopedAction(uint8_t action);

  // Enforce settings whose values depend on others (e.g. sleepScreen=QUICK_RESUME implies
  // quickResumeSleepScreen=ON). Call after any setting mutation that could invalidate the pair.
  static void normalizeDependentSettings(CrossPointSettings& settings);

  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

// Outside the class because a constexpr member cannot be called in a static_assert within its
// own definition -- the class is still incomplete there.
static_assert(CrossPointSettings::FONT_SIZE_RUNG_COUNT == static_cast<int>(CrossPointSettings::FONT_SIZE_COUNT) &&
                  CrossPointSettings::ladderCoversEveryFontSize(),
              "every FONT_SIZE must appear exactly once in FONT_SIZE_RUNGS; a FONT_SIZE without a rung has no "
              "point size, and builtinRungForPoints() could never choose it");
static_assert(CrossPointSettings::fontSizePoints(CrossPointSettings::builtinRungForPoints(
                  CrossPointSettings::BUILTIN_FACE_MAX_POINTS)) == CrossPointSettings::BUILTIN_FACE_MAX_POINTS,
              "the scaling master must be a rung of its own; the sizes above it are drawn from its faces");

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
