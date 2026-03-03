#include "CrossPointSettings.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstring>

#include "FeatureFlags.h"
#include "fontIds.h"

CrossPointSettings CrossPointSettings::instance;

static bool readAndValidate(FsFile& file, uint8_t& member, const uint8_t maxValue) {
  uint8_t tempValue = 0;
  if (!serialization::readPod(file, tempValue)) {
    return false;
  }
  if (tempValue < maxValue) {
    member = tempValue;
  }
  return true;
}

void applyLegacyStatusBarSettings(CrossPointSettings& settings) {
  settings.statusBarProgressBarThickness = CrossPointSettings::PROGRESS_BAR_NORMAL;
  switch (static_cast<CrossPointSettings::STATUS_BAR_MODE>(settings.statusBar)) {
    case CrossPointSettings::NONE:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::HIDE_TITLE;
      settings.statusBarBattery = 0;
      break;
    case CrossPointSettings::NO_PROGRESS:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::BOOK_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::ONLY_BOOK_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::HIDE_TITLE;
      settings.statusBarBattery = 0;
      break;
    case CrossPointSettings::CHAPTER_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 1;
      settings.statusBarProgressBar = CrossPointSettings::CHAPTER_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::FULL:
    default:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 1;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
  }
}

namespace {
constexpr uint8_t SETTINGS_FILE_VERSION = 4;
// Increment this when adding new persisted settings fields.
// Must match the number of writePod/writeString calls in saveToFile() (excluding the
// version and count header writes). Current count: 39.
constexpr uint8_t SETTINGS_COUNT = 39;
constexpr char SETTINGS_FILE_BIN[] = "/.crosspoint/settings.bin";
constexpr char SETTINGS_FILE_JSON[] = "/.crosspoint/settings.json";
constexpr char SETTINGS_FILE_BAK[] = "/.crosspoint/settings.bin.bak";

void applyLegacyFrontButtonLayout(CrossPointSettings& settings) {
  settings.applyFrontButtonLayoutPreset(
      static_cast<CrossPointSettings::FRONT_BUTTON_LAYOUT>(settings.frontButtonLayout));
}

}  // namespace

void CrossPointSettings::validateFrontButtonMapping(CrossPointSettings& settings) {
  const uint8_t mapping[] = {settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                             settings.frontButtonRight};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        settings.frontButtonBack = FRONT_HW_BACK;
        settings.frontButtonConfirm = FRONT_HW_CONFIRM;
        settings.frontButtonLeft = FRONT_HW_LEFT;
        settings.frontButtonRight = FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

bool CrossPointSettings::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveSettings(*this, SETTINGS_FILE_JSON);
}

bool CrossPointSettings::loadFromFile() {
  if (Storage.exists(SETTINGS_FILE_JSON)) {
    String json = Storage.readFile(SETTINGS_FILE_JSON);
    if (!json.isEmpty()) {
      bool resave = false;
      bool result = JsonSettingsIO::loadSettings(*this, json.c_str(), &resave);
      if (result) {
        validateAndClamp();
        if (resave) {
          if (saveToFile()) {
            LOG_DBG("CPS", "Resaved settings to update format");
          } else {
            LOG_ERR("CPS", "Failed to resave settings after format update");
          }
        }
      }
      return result;
    }
  }

  // Fall back to binary migration
  if (Storage.exists(SETTINGS_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      if (saveToFile()) {
        Storage.rename(SETTINGS_FILE_BIN, SETTINGS_FILE_BAK);
        LOG_DBG("CPS", "Migrated settings.bin to settings.json");
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save migrated settings to JSON");
      }
    }
  }

  return false;
}

bool CrossPointSettings::loadFromBinaryFile() {
  FsFile inputFile;
  if (!Storage.openFileForRead("CPS", SETTINGS_FILE_BIN, inputFile)) {
    return false;
  }

  uint8_t version = 0;
  if (!serialization::readPod(inputFile, version)) {
    LOG_ERR("CPS", "Deserialization failed: Could not read version");
    inputFile.close();
    return false;
  }
  if (version < 1 || version > SETTINGS_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    inputFile.close();
    return false;
  }

  uint8_t fileSettingsCount = 0;
  if (!serialization::readPod(inputFile, fileSettingsCount)) {
    LOG_ERR("CPS", "Deserialization failed: Could not read setting count");
    inputFile.close();
    return false;
  }
  if (fileSettingsCount > SETTINGS_COUNT) {
    LOG_WRN("CPS", "Settings count %u exceeds supported %u, truncating", fileSettingsCount, SETTINGS_COUNT);
    fileSettingsCount = SETTINGS_COUNT;
  }

  uint8_t settingsRead = 0;
  bool frontButtonMappingRead = false;
  do {
    serialization::readPod(inputFile, sleepScreen);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, extraParagraphSpacing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, shortPwrBtn, SHORT_PWRBTN_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, statusBar, STATUS_BAR_MODE_COUNT);  // legacy
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, orientation, ORIENTATION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLayout, FRONT_BUTTON_LAYOUT_COUNT);  // legacy
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sideButtonLayout, SIDE_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, fontFamily, FONT_FAMILY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, fontSize, FONT_SIZE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, lineSpacing, LINE_COMPRESSION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, paragraphAlignment, PARAGRAPH_ALIGNMENT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepTimeout, SLEEP_TIMEOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, refreshFrequency, REFRESH_FREQUENCY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, screenMargin);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverMode, SLEEP_SCREEN_COVER_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string urlStr;
      serialization::readString(inputFile, urlStr);
      strncpy(opdsServerUrl, urlStr.c_str(), sizeof(opdsServerUrl) - 1);
      opdsServerUrl[sizeof(opdsServerUrl) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, textAntiAliasing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, hideBatteryPercentage, HIDE_BATTERY_PERCENTAGE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, longPressChapterSkip);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, hyphenationEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string usernameStr;
      serialization::readString(inputFile, usernameStr);
      strncpy(opdsUsername, usernameStr.c_str(), sizeof(opdsUsername) - 1);
      opdsUsername[sizeof(opdsUsername) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string passwordStr;
      serialization::readString(inputFile, passwordStr);
      strncpy(opdsPassword, passwordStr.c_str(), sizeof(opdsPassword) - 1);
      opdsPassword[sizeof(opdsPassword) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverFilter, SLEEP_SCREEN_COVER_FILTER_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, backgroundServerOnCharge);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, todoFallbackCover);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, timeMode);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, timeZoneOffset);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, lastTimeSyncEpoch);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, releaseChannel, RELEASE_CHANNEL_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenSource, SLEEP_SCREEN_SOURCE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    if (version >= 2) {
      std::string pathStr;
      serialization::readString(inputFile, pathStr);
      strncpy(userFontPath, pathStr.c_str(), sizeof(userFontPath) - 1);
      userFontPath[sizeof(userFontPath) - 1] = '\0';
      if (++settingsRead >= fileSettingsCount) break;
    }
    if (version >= 3) {
      serialization::readPod(inputFile, usbMscPromptOnConnect);
      if (++settingsRead >= fileSettingsCount) break;
    }
    if (version >= 4) {
      std::string selectedOtaBundleStr;
      serialization::readString(inputFile, selectedOtaBundleStr);
      strncpy(selectedOtaBundle, selectedOtaBundleStr.c_str(), sizeof(selectedOtaBundle) - 1);
      selectedOtaBundle[sizeof(selectedOtaBundle) - 1] = '\0';
      if (++settingsRead >= fileSettingsCount) break;

      std::string installedOtaBundleStr;
      serialization::readString(inputFile, installedOtaBundleStr);
      strncpy(installedOtaBundle, installedOtaBundleStr.c_str(), sizeof(installedOtaBundle) - 1);
      installedOtaBundle[sizeof(installedOtaBundle) - 1] = '\0';
      if (++settingsRead >= fileSettingsCount) break;

      std::string installedOtaFeatureFlagsStr;
      serialization::readString(inputFile, installedOtaFeatureFlagsStr);
      strncpy(installedOtaFeatureFlags, installedOtaFeatureFlagsStr.c_str(), sizeof(installedOtaFeatureFlags) - 1);
      installedOtaFeatureFlags[sizeof(installedOtaFeatureFlags) - 1] = '\0';
      if (++settingsRead >= fileSettingsCount) break;
    }

    const bool backRead = readAndValidate(inputFile, frontButtonBack, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    const bool confirmRead = readAndValidate(inputFile, frontButtonConfirm, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    const bool leftRead = readAndValidate(inputFile, frontButtonLeft, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    const bool rightRead = readAndValidate(inputFile, frontButtonRight, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    frontButtonMappingRead = backRead && confirmRead && leftRead && rightRead;
  } while (false);

  if (frontButtonMappingRead) {
    CrossPointSettings::validateFrontButtonMapping(*this);
  } else {
    applyLegacyFrontButtonLayout(*this);
  }

  // Binary settings only carry the legacy statusBar enum.
  applyLegacyStatusBarSettings(*this);

  validateAndClamp();
  inputFile.close();
  LOG_DBG("CPS", "Settings loaded from binary file");
  return true;
}

void CrossPointSettings::applyFrontButtonLayoutPreset(const FRONT_BUTTON_LAYOUT layout) {
  frontButtonLayout = static_cast<uint8_t>(layout);

  switch (layout) {
    case LEFT_RIGHT_BACK_CONFIRM:
      frontButtonBack = FRONT_HW_LEFT;
      frontButtonConfirm = FRONT_HW_RIGHT;
      frontButtonLeft = FRONT_HW_BACK;
      frontButtonRight = FRONT_HW_CONFIRM;
      break;
    case LEFT_BACK_CONFIRM_RIGHT:
      frontButtonBack = FRONT_HW_CONFIRM;
      frontButtonConfirm = FRONT_HW_LEFT;
      frontButtonLeft = FRONT_HW_BACK;
      frontButtonRight = FRONT_HW_RIGHT;
      break;
    case BACK_CONFIRM_RIGHT_LEFT:
      frontButtonBack = FRONT_HW_BACK;
      frontButtonConfirm = FRONT_HW_CONFIRM;
      frontButtonLeft = FRONT_HW_RIGHT;
      frontButtonRight = FRONT_HW_LEFT;
      break;
    case LEFT_LEFT_RIGHT_RIGHT:
    case BACK_CONFIRM_LEFT_RIGHT:
    default:
      frontButtonBack = FRONT_HW_BACK;
      frontButtonConfirm = FRONT_HW_CONFIRM;
      frontButtonLeft = FRONT_HW_LEFT;
      frontButtonRight = FRONT_HW_RIGHT;
      break;
  }
}

void CrossPointSettings::enforceButtonLayoutConstraints() {
  if (frontButtonLayout == LEFT_LEFT_RIGHT_RIGHT) {
    shortPwrBtn = SELECT;
  }
}

void CrossPointSettings::validateAndClamp() {
  // Migrate legacy raw values (enum constants moved; use integer literals to be safe).
  if (sleepScreen == 3 /* old COVER */ || sleepScreen == 5 /* old COVER_CUSTOM */) {
    sleepScreen = CUSTOM;
  } else if (sleepScreen == 4 /* old BLANK */) {
    sleepScreen = DARK;
  } else if (sleepScreen == 6 /* old TRANSPARENT */) {
    sleepScreen = TRANSPARENT;  // = 3
  } else if (sleepScreen >= SLEEP_SCREEN_MODE_COUNT) {
    sleepScreen = DARK;
  }
  if (sleepScreenCoverMode > CROP) sleepScreenCoverMode = FIT;
  if (sleepScreenSource >= SLEEP_SCREEN_SOURCE_COUNT) sleepScreenSource = SLEEP_SOURCE_SLEEP;
  if (statusBar >= STATUS_BAR_MODE_COUNT) statusBar = FULL;
  if (statusBarProgressBar >= STATUS_BAR_PROGRESS_BAR_COUNT) statusBarProgressBar = HIDE_PROGRESS;
  if (statusBarProgressBarThickness >= STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT) {
    statusBarProgressBarThickness = PROGRESS_BAR_NORMAL;
  }
  if (statusBarTitle >= STATUS_BAR_TITLE_COUNT) statusBarTitle = CHAPTER_TITLE;
  if (orientation > LANDSCAPE_CCW) orientation = PORTRAIT;
  if (frontButtonLayout > LEFT_LEFT_RIGHT_RIGHT) frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  if (sideButtonLayout > NEXT_PREV) sideButtonLayout = PREV_NEXT;
  if (fontFamily >= FONT_FAMILY_COUNT) fontFamily = BOOKERLY;
  if (fontSize > EXTRA_LARGE) fontSize = MEDIUM;
#if !ENABLE_EXTENDED_FONTS
  if (fontFamily != USER_SD) fontFamily = BOOKERLY;
  fontSize = MEDIUM;
#elif !ENABLE_OPENDYSLEXIC_FONTS
  if (fontFamily == OPENDYSLEXIC) fontFamily = NOTOSANS;
#endif
#if !ENABLE_USER_FONTS
  if (fontFamily == USER_SD) fontFamily = BOOKERLY;
#endif
  if (lineSpacing > WIDE) lineSpacing = NORMAL;
  if (paragraphAlignment >= PARAGRAPH_ALIGNMENT_COUNT) paragraphAlignment = JUSTIFIED;
  if (sleepTimeout > SLEEP_30_MIN) sleepTimeout = SLEEP_10_MIN;
  if (refreshFrequency > REFRESH_30) refreshFrequency = REFRESH_15;
  if (shortPwrBtn > SELECT) shortPwrBtn = IGNORE;
  if (hideBatteryPercentage > HIDE_ALWAYS) hideBatteryPercentage = HIDE_NEVER;
  if (timeMode > TIME_MODE_MANUAL) timeMode = TIME_MODE_UTC;
  if (todoFallbackCover > 1) todoFallbackCover = 0;
  if (releaseChannel >= RELEASE_CHANNEL_COUNT) releaseChannel = RELEASE_STABLE;

  if (uiTheme > FORK_DRIFT) uiTheme = LYRA;
#if !ENABLE_LYRA_THEME
  uiTheme = CLASSIC;
#endif

  if (timeZoneOffset > 26) timeZoneOffset = 12;
  if (screenMargin < 5 || screenMargin > 40) screenMargin = 5;

  extraParagraphSpacing = extraParagraphSpacing ? 1 : 0;
  textAntiAliasing = textAntiAliasing ? 1 : 0;
  hyphenationEnabled = hyphenationEnabled ? 1 : 0;
  longPressChapterSkip = longPressChapterSkip ? 1 : 0;
  statusBarChapterPageCount = statusBarChapterPageCount ? 1 : 0;
  statusBarBookProgressPercentage = statusBarBookProgressPercentage ? 1 : 0;
  statusBarBattery = statusBarBattery ? 1 : 0;
  backgroundServerOnCharge = backgroundServerOnCharge ? 1 : 0;
  wifiAutoConnect = wifiAutoConnect ? 1 : 0;
  if (wifiAutoConnect) {
    backgroundServerOnCharge = 1;
  }
  usbMscPromptOnConnect = usbMscPromptOnConnect ? 1 : 0;

  // Sanitize deviceName: keep only [a-z0-9-], lowercase, max 24 usable chars.
  {
    char sanitized[25] = {};
    size_t out = 0;
    for (size_t i = 0; deviceName[i] != '\0' && out < 24; ++i) {
      const char c = static_cast<char>(tolower(static_cast<unsigned char>(deviceName[i])));
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
        sanitized[out++] = c;
      }
    }
    // Strip leading/trailing hyphens
    size_t start = 0;
    while (start < out && sanitized[start] == '-') ++start;
    while (out > start && sanitized[out - 1] == '-') --out;
    memmove(sanitized, sanitized + start, out - start);
    out -= start;
    sanitized[out] = '\0';
    strncpy(deviceName, sanitized, sizeof(deviceName) - 1);
    deviceName[sizeof(deviceName) - 1] = '\0';
  }

  enforceButtonLayoutConstraints();
}

float CrossPointSettings::getReaderLineCompression() const {
  switch (fontFamily) {
    case BOOKERLY:
    default:
      switch (lineSpacing) {
        case TIGHT:
          return 0.95f;
        case NORMAL:
        default:
          return 1.0f;
        case WIDE:
          return 1.1f;
      }
    case NOTOSANS:
      switch (lineSpacing) {
        case TIGHT:
          return 0.90f;
        case NORMAL:
        default:
          return 0.95f;
        case WIDE:
          return 1.0f;
      }
    case OPENDYSLEXIC:
      switch (lineSpacing) {
        case TIGHT:
          return 0.90f;
        case NORMAL:
        default:
          return 0.95f;
        case WIDE:
          return 1.0f;
      }
    case USER_SD:
      switch (lineSpacing) {
        case TIGHT:
          return 0.90f;
        case NORMAL:
        default:
          return 1.0f;
        case WIDE:
          return 1.1f;
      }
  }
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  switch (sleepTimeout) {
    case SLEEP_1_MIN:
      return 1UL * 60 * 1000;
    case SLEEP_5_MIN:
      return 5UL * 60 * 1000;
    case SLEEP_10_MIN:
    default:
      return 10UL * 60 * 1000;
    case SLEEP_15_MIN:
      return 15UL * 60 * 1000;
    case SLEEP_30_MIN:
      return 30UL * 60 * 1000;
  }
}

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
  }
}

int CrossPointSettings::getTimeZoneOffsetSeconds() const {
  const int offsetHours = static_cast<int>(timeZoneOffset) - 12;
  return offsetHours * 3600;
}

int CrossPointSettings::getReaderFontId() const {
#if !ENABLE_EXTENDED_FONTS
  return BOOKERLY_14_FONT_ID;
#else
  uint8_t effectiveFamily = fontFamily;
#if !ENABLE_OPENDYSLEXIC_FONTS
  if (effectiveFamily == OPENDYSLEXIC) effectiveFamily = NOTOSANS;
#endif
#if !ENABLE_NOTOSANS_FONTS
  if (effectiveFamily == NOTOSANS) effectiveFamily = BOOKERLY;
#endif
#if !ENABLE_USER_FONTS
  if (effectiveFamily == USER_SD) effectiveFamily = BOOKERLY;
#endif

  switch (effectiveFamily) {
    case USER_SD:
#if ENABLE_USER_FONTS
      return USER_SD_FONT_ID;
#else
      return BOOKERLY_14_FONT_ID;
#endif
    case BOOKERLY:
    default:
#if ENABLE_BOOKERLY_FONTS
      switch (fontSize) {
        case SMALL:
          return BOOKERLY_12_FONT_ID;
        case MEDIUM:
        default:
          return BOOKERLY_14_FONT_ID;
        case LARGE:
          return BOOKERLY_16_FONT_ID;
        case EXTRA_LARGE:
          return BOOKERLY_18_FONT_ID;
      }
#else
      return BOOKERLY_14_FONT_ID;
#endif
    case NOTOSANS:
#if ENABLE_NOTOSANS_FONTS
      switch (fontSize) {
        case SMALL:
          return NOTOSANS_12_FONT_ID;
        case MEDIUM:
        default:
          return NOTOSANS_14_FONT_ID;
        case LARGE:
          return NOTOSANS_16_FONT_ID;
        case EXTRA_LARGE:
          return NOTOSANS_18_FONT_ID;
      }
#else
      return BOOKERLY_14_FONT_ID;
#endif
    case OPENDYSLEXIC:
#if ENABLE_OPENDYSLEXIC_FONTS
      switch (fontSize) {
        case SMALL:
          return OPENDYSLEXIC_8_FONT_ID;
        case MEDIUM:
        default:
          return OPENDYSLEXIC_10_FONT_ID;
        case LARGE:
          return OPENDYSLEXIC_12_FONT_ID;
        case EXTRA_LARGE:
          return OPENDYSLEXIC_14_FONT_ID;
      }
#else
      return BOOKERLY_14_FONT_ID;
#endif
  }
#endif
}
