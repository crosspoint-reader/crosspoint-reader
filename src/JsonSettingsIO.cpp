#include "JsonSettingsIO.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "RecentBooksStore.h"
#include "WifiCredentialStore.h"

// ---- CrossPointState ----

bool JsonSettingsIO::saveState(const CrossPointState& s, const char* path) {
  JsonDocument doc;
  doc["openEpubPath"] = s.openEpubPath;
  doc["lastSleepImage"] = s.lastSleepImage;
  doc["readerActivityLoadCount"] = s.readerActivityLoadCount;
  doc["lastSleepFromReader"] = s.lastSleepFromReader;
  doc["wifiAutoConnectSkipCount"] = s.wifiAutoConnectSkipCount;
  doc["wifiAutoConnectBackoffLevel"] = s.wifiAutoConnectBackoffLevel;
  doc["wifiAutoConnectWaitingForNewCredential"] = s.wifiAutoConnectWaitingForNewCredential;

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadState(CrossPointState& s, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("CPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  s.openEpubPath = doc["openEpubPath"] | std::string("");
  s.lastSleepImage = doc["lastSleepImage"] | (uint8_t)0;
  s.readerActivityLoadCount = doc["readerActivityLoadCount"] | (uint8_t)0;
  s.lastSleepFromReader = doc["lastSleepFromReader"] | false;
  s.wifiAutoConnectSkipCount = doc["wifiAutoConnectSkipCount"] | (uint8_t)0;
  s.wifiAutoConnectBackoffLevel = doc["wifiAutoConnectBackoffLevel"] | (uint8_t)0;
  s.wifiAutoConnectWaitingForNewCredential = doc["wifiAutoConnectWaitingForNewCredential"] | false;
  return true;
}

// ---- CrossPointSettings ----

bool JsonSettingsIO::saveSettings(const CrossPointSettings& s, const char* path) {
  JsonDocument doc;

  doc["sleepScreen"] = s.sleepScreen;
  doc["sleepScreenSource"] = s.sleepScreenSource;
  doc["sleepPinnedPath"] = s.sleepPinnedPath;
  doc["sleepScreenCoverMode"] = s.sleepScreenCoverMode;
  doc["sleepScreenCoverFilter"] = s.sleepScreenCoverFilter;
  doc["statusBar"] = s.statusBar;
  doc["statusBarChapterPageCount"] = s.statusBarChapterPageCount;
  doc["statusBarBookProgressPercentage"] = s.statusBarBookProgressPercentage;
  doc["statusBarProgressBar"] = s.statusBarProgressBar;
  doc["statusBarProgressBarThickness"] = s.statusBarProgressBarThickness;
  doc["statusBarTitle"] = s.statusBarTitle;
  doc["statusBarBattery"] = s.statusBarBattery;
  doc["extraParagraphSpacing"] = s.extraParagraphSpacing;
  doc["textAntiAliasing"] = s.textAntiAliasing;
  doc["shortPwrBtn"] = s.shortPwrBtn;
  doc["orientation"] = s.orientation;
  doc["frontButtonLayout"] = s.frontButtonLayout;
  doc["sideButtonLayout"] = s.sideButtonLayout;
  doc["frontButtonBack"] = s.frontButtonBack;
  doc["frontButtonConfirm"] = s.frontButtonConfirm;
  doc["frontButtonLeft"] = s.frontButtonLeft;
  doc["frontButtonRight"] = s.frontButtonRight;
  doc["fontFamily"] = s.fontFamily;
  doc["fontSize"] = s.fontSize;
  doc["lineSpacing"] = s.lineSpacing;
  doc["paragraphAlignment"] = s.paragraphAlignment;
  doc["sleepTimeout"] = s.sleepTimeout;
  doc["refreshFrequency"] = s.refreshFrequency;
  doc["screenMargin"] = s.screenMargin;
  doc["opdsServerUrl"] = s.opdsServerUrl;
  doc["opdsUsername"] = s.opdsUsername;
  doc["opdsPassword_obf"] = obfuscation::obfuscateToBase64(s.opdsPassword);
  doc["hideBatteryPercentage"] = s.hideBatteryPercentage;
  doc["longPressChapterSkip"] = s.longPressChapterSkip;
  doc["hyphenationEnabled"] = s.hyphenationEnabled;
  doc["backgroundServerOnCharge"] = s.backgroundServerOnCharge;
  doc["todoFallbackCover"] = s.todoFallbackCover;
  doc["timeMode"] = s.timeMode;
  doc["timeZoneOffset"] = s.timeZoneOffset;
  doc["lastTimeSyncEpoch"] = s.lastTimeSyncEpoch;
  doc["releaseChannel"] = s.releaseChannel;
  doc["uiTheme"] = s.uiTheme;
  doc["fadingFix"] = s.fadingFix;
  doc["darkMode"] = s.darkMode;
  doc["embeddedStyle"] = s.embeddedStyle;
  doc["usbMscPromptOnConnect"] = s.usbMscPromptOnConnect;
  doc["userFontPath"] = s.userFontPath;
  doc["selectedOtaBundle"] = s.selectedOtaBundle;
  doc["installedOtaBundle"] = s.installedOtaBundle;
  doc["installedOtaFeatureFlags"] = s.installedOtaFeatureFlags;
  doc["deviceName"] = s.deviceName;
  doc["wifiAutoConnect"] = s.wifiAutoConnect;
  doc["showHiddenFiles"] = s.showHiddenFiles;
  doc["imageRendering"] = s.imageRendering;
  doc["globalStatusBar"] = s.globalStatusBar;
  doc["globalStatusBarPosition"] = s.globalStatusBarPosition;

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadSettings(CrossPointSettings& s, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("CPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  using S = CrossPointSettings;
  auto clamp = [](uint8_t val, uint8_t maxVal, uint8_t def) -> uint8_t { return val < maxVal ? val : def; };

  s.sleepScreen = clamp(doc["sleepScreen"] | (uint8_t)S::DARK, S::SLEEP_SCREEN_MODE_COUNT, S::DARK);
  s.sleepScreenSource = clamp(doc["sleepScreenSource"] | (uint8_t)S::SLEEP_SOURCE_SLEEP, S::SLEEP_SCREEN_SOURCE_COUNT,
                              S::SLEEP_SOURCE_SLEEP);
  const char* sleepPinnedPath = doc["sleepPinnedPath"] | "";
  strncpy(s.sleepPinnedPath, sleepPinnedPath, sizeof(s.sleepPinnedPath) - 1);
  s.sleepPinnedPath[sizeof(s.sleepPinnedPath) - 1] = '\0';

  s.sleepScreenCoverMode =
      clamp(doc["sleepScreenCoverMode"] | (uint8_t)S::FIT, S::SLEEP_SCREEN_COVER_MODE_COUNT, S::FIT);
  s.sleepScreenCoverFilter =
      clamp(doc["sleepScreenCoverFilter"] | (uint8_t)S::NO_FILTER, S::SLEEP_SCREEN_COVER_FILTER_COUNT, S::NO_FILTER);
  s.statusBar = clamp(doc["statusBar"] | (uint8_t)S::FULL, S::STATUS_BAR_MODE_COUNT, S::FULL);
  if (doc["statusBarChapterPageCount"].isNull()) {
    applyLegacyStatusBarSettings(s);
  } else {
    s.statusBarChapterPageCount = doc["statusBarChapterPageCount"] | (uint8_t)1;
    s.statusBarBookProgressPercentage = doc["statusBarBookProgressPercentage"] | (uint8_t)1;
    s.statusBarProgressBar = clamp(doc["statusBarProgressBar"] | (uint8_t)S::HIDE_PROGRESS,
                                   S::STATUS_BAR_PROGRESS_BAR_COUNT, S::HIDE_PROGRESS);
    s.statusBarProgressBarThickness = clamp(doc["statusBarProgressBarThickness"] | (uint8_t)S::PROGRESS_BAR_NORMAL,
                                            S::STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT, S::PROGRESS_BAR_NORMAL);
    s.statusBarTitle =
        clamp(doc["statusBarTitle"] | (uint8_t)S::CHAPTER_TITLE, S::STATUS_BAR_TITLE_COUNT, S::CHAPTER_TITLE);
    s.statusBarBattery = doc["statusBarBattery"] | (uint8_t)1;
  }
  s.extraParagraphSpacing = doc["extraParagraphSpacing"] | (uint8_t)1;
  s.textAntiAliasing = doc["textAntiAliasing"] | (uint8_t)1;
  s.shortPwrBtn = clamp(doc["shortPwrBtn"] | (uint8_t)S::IGNORE, S::SHORT_PWRBTN_COUNT, S::IGNORE);
  s.orientation = clamp(doc["orientation"] | (uint8_t)S::PORTRAIT, S::ORIENTATION_COUNT, S::PORTRAIT);
  s.frontButtonLayout = clamp(doc["frontButtonLayout"] | (uint8_t)S::BACK_CONFIRM_LEFT_RIGHT,
                              S::FRONT_BUTTON_LAYOUT_COUNT, S::BACK_CONFIRM_LEFT_RIGHT);
  s.sideButtonLayout =
      clamp(doc["sideButtonLayout"] | (uint8_t)S::PREV_NEXT, S::SIDE_BUTTON_LAYOUT_COUNT, S::PREV_NEXT);
  const bool hasFrontButtonMapping = !(doc["frontButtonBack"].isNull() || doc["frontButtonConfirm"].isNull() ||
                                       doc["frontButtonLeft"].isNull() || doc["frontButtonRight"].isNull());
  if (hasFrontButtonMapping) {
    s.frontButtonBack =
        clamp(doc["frontButtonBack"] | (uint8_t)S::FRONT_HW_BACK, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_BACK);
    s.frontButtonConfirm = clamp(doc["frontButtonConfirm"] | (uint8_t)S::FRONT_HW_CONFIRM,
                                 S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_CONFIRM);
    s.frontButtonLeft =
        clamp(doc["frontButtonLeft"] | (uint8_t)S::FRONT_HW_LEFT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_LEFT);
    s.frontButtonRight =
        clamp(doc["frontButtonRight"] | (uint8_t)S::FRONT_HW_RIGHT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_RIGHT);
    CrossPointSettings::validateFrontButtonMapping(s);
  } else {
    s.applyFrontButtonLayoutPreset(static_cast<S::FRONT_BUTTON_LAYOUT>(s.frontButtonLayout));
  }
  s.fontFamily = clamp(doc["fontFamily"] | (uint8_t)S::BOOKERLY, S::FONT_FAMILY_COUNT, S::BOOKERLY);
  s.fontSize = clamp(doc["fontSize"] | (uint8_t)S::MEDIUM, S::FONT_SIZE_COUNT, S::MEDIUM);
  s.lineSpacing = clamp(doc["lineSpacing"] | (uint8_t)S::NORMAL, S::LINE_COMPRESSION_COUNT, S::NORMAL);
  s.paragraphAlignment =
      clamp(doc["paragraphAlignment"] | (uint8_t)S::JUSTIFIED, S::PARAGRAPH_ALIGNMENT_COUNT, S::JUSTIFIED);
  s.sleepTimeout = clamp(doc["sleepTimeout"] | (uint8_t)S::SLEEP_10_MIN, S::SLEEP_TIMEOUT_COUNT, S::SLEEP_10_MIN);
  s.refreshFrequency =
      clamp(doc["refreshFrequency"] | (uint8_t)S::REFRESH_15, S::REFRESH_FREQUENCY_COUNT, S::REFRESH_15);
  s.screenMargin = doc["screenMargin"] | (uint8_t)5;
  s.hideBatteryPercentage =
      clamp(doc["hideBatteryPercentage"] | (uint8_t)S::HIDE_NEVER, S::HIDE_BATTERY_PERCENTAGE_COUNT, S::HIDE_NEVER);
  s.longPressChapterSkip = doc["longPressChapterSkip"] | (uint8_t)1;
  s.hyphenationEnabled = doc["hyphenationEnabled"] | (uint8_t)0;
  s.backgroundServerOnCharge = doc["backgroundServerOnCharge"] | (uint8_t)0;
  s.todoFallbackCover = doc["todoFallbackCover"] | (uint8_t)0;
  s.timeMode = clamp(doc["timeMode"] | (uint8_t)S::TIME_MODE_UTC, static_cast<uint8_t>(S::TIME_MODE_MANUAL + 1),
                     S::TIME_MODE_UTC);
  s.timeZoneOffset = doc["timeZoneOffset"] | (uint8_t)12;
  s.lastTimeSyncEpoch = doc["lastTimeSyncEpoch"] | (uint32_t)0;
  s.releaseChannel =
      clamp(doc["releaseChannel"] | (uint8_t)S::RELEASE_STABLE, S::RELEASE_CHANNEL_COUNT, S::RELEASE_STABLE);
  s.uiTheme = clamp(doc["uiTheme"] | (uint8_t)S::LYRA, static_cast<uint8_t>(S::POKEMON_PARTY + 1), S::LYRA);
  s.fadingFix = doc["fadingFix"] | (uint8_t)0;
  s.darkMode = doc["darkMode"] | (uint8_t)0;
  s.embeddedStyle = doc["embeddedStyle"] | (uint8_t)1;
  s.usbMscPromptOnConnect = doc["usbMscPromptOnConnect"] | (uint8_t)0;
  s.wifiAutoConnect = doc["wifiAutoConnect"] | (uint8_t)0;
  s.showHiddenFiles = doc["showHiddenFiles"] | (uint8_t)0;
  s.imageRendering = clamp(doc["imageRendering"] | (uint8_t)S::IMAGES_DISPLAY, S::IMAGE_RENDERING_COUNT, S::IMAGES_DISPLAY);
  s.globalStatusBar = doc["globalStatusBar"] | (uint8_t)0;
  s.globalStatusBarPosition =
      clamp(doc["globalStatusBarPosition"] | (uint8_t)S::STATUS_BAR_TOP, S::GLOBAL_STATUS_BAR_POSITION_COUNT, S::STATUS_BAR_TOP);

  const char* url = doc["opdsServerUrl"] | "";
  strncpy(s.opdsServerUrl, url, sizeof(s.opdsServerUrl) - 1);
  s.opdsServerUrl[sizeof(s.opdsServerUrl) - 1] = '\0';

  const char* user = doc["opdsUsername"] | "";
  strncpy(s.opdsUsername, user, sizeof(s.opdsUsername) - 1);
  s.opdsUsername[sizeof(s.opdsUsername) - 1] = '\0';

  bool passOk = false;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["opdsPassword_obf"] | "", &passOk);
  if (!passOk || pass.empty()) {
    pass = doc["opdsPassword"] | "";
    if (!pass.empty() && needsResave) *needsResave = true;
  }
  strncpy(s.opdsPassword, pass.c_str(), sizeof(s.opdsPassword) - 1);
  s.opdsPassword[sizeof(s.opdsPassword) - 1] = '\0';

  const char* userFontPath = doc["userFontPath"] | "";
  strncpy(s.userFontPath, userFontPath, sizeof(s.userFontPath) - 1);
  s.userFontPath[sizeof(s.userFontPath) - 1] = '\0';

  const char* selectedOtaBundle = doc["selectedOtaBundle"] | "";
  strncpy(s.selectedOtaBundle, selectedOtaBundle, sizeof(s.selectedOtaBundle) - 1);
  s.selectedOtaBundle[sizeof(s.selectedOtaBundle) - 1] = '\0';

  const char* installedOtaBundle = doc["installedOtaBundle"] | "";
  strncpy(s.installedOtaBundle, installedOtaBundle, sizeof(s.installedOtaBundle) - 1);
  s.installedOtaBundle[sizeof(s.installedOtaBundle) - 1] = '\0';

  const char* installedOtaFeatureFlags = doc["installedOtaFeatureFlags"] | "";
  strncpy(s.installedOtaFeatureFlags, installedOtaFeatureFlags, sizeof(s.installedOtaFeatureFlags) - 1);
  s.installedOtaFeatureFlags[sizeof(s.installedOtaFeatureFlags) - 1] = '\0';

  const char* deviceName = doc["deviceName"] | "";
  strncpy(s.deviceName, deviceName, sizeof(s.deviceName) - 1);
  s.deviceName[sizeof(s.deviceName) - 1] = '\0';

  LOG_DBG("CPS", "Settings loaded from file");
  return true;
}

// ---- KOReaderCredentialStore ----

bool JsonSettingsIO::saveKOReader(const KOReaderCredentialStore& store, const char* path) {
  JsonDocument doc;
  doc["username"] = store.getUsername();
  doc["password_obf"] = obfuscation::obfuscateToBase64(store.getPassword());
  doc["serverUrl"] = store.getServerUrl();
  doc["matchMethod"] = static_cast<uint8_t>(store.getMatchMethod());

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadKOReader(KOReaderCredentialStore& store, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("KRS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.username = doc["username"] | std::string("");
  bool ok = false;
  store.password = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &ok);
  if (!ok || store.password.empty()) {
    store.password = doc["password"] | std::string("");
    if (!store.password.empty() && needsResave) *needsResave = true;
  }
  store.serverUrl = doc["serverUrl"] | std::string("");
  uint8_t method = doc["matchMethod"] | (uint8_t)0;
  store.matchMethod = static_cast<DocumentMatchMethod>(method);

  LOG_DBG("KRS", "Loaded KOReader credentials for user: %s", store.username.c_str());
  return true;
}

// ---- WifiCredentialStore ----

bool JsonSettingsIO::saveWifi(const WifiCredentialStore& store, const char* path) {
  JsonDocument doc;
  doc["lastConnectedSsid"] = store.getLastConnectedSsid();

  JsonArray arr = doc["credentials"].to<JsonArray>();
  for (const auto& cred : store.getCredentials()) {
    JsonObject obj = arr.add<JsonObject>();
    obj["ssid"] = cred.ssid;
    obj["password_obf"] = obfuscation::obfuscateToBase64(cred.password);
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadWifi(WifiCredentialStore& store, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("WCS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.lastConnectedSsid = doc["lastConnectedSsid"] | std::string("");

  store.credentials.clear();
  JsonArray arr = doc["credentials"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (store.credentials.size() >= store.MAX_NETWORKS) break;
    WifiCredential cred;
    cred.ssid = obj["ssid"] | std::string("");
    bool ok = false;
    cred.password = obfuscation::deobfuscateFromBase64(obj["password_obf"] | "", &ok);
    if (!ok || cred.password.empty()) {
      cred.password = obj["password"] | std::string("");
      if (!cred.password.empty() && needsResave) *needsResave = true;
    }
    store.credentials.push_back(cred);
  }

  LOG_DBG("WCS", "Loaded %zu WiFi credentials from file", store.credentials.size());
  return true;
}

// ---- RecentBooksStore ----

bool JsonSettingsIO::saveRecentBooks(const RecentBooksStore& store, const char* path) {
  JsonDocument doc;
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : store.getBooks()) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["coverBmpPath"] = book.coverBmpPath;
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadRecentBooks(RecentBooksStore& store, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("RBS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.recentBooks.clear();
  JsonArray arr = doc["books"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (store.getCount() >= 10) break;
    RecentBook book;
    book.path = obj["path"] | std::string("");
    book.title = obj["title"] | std::string("");
    book.author = obj["author"] | std::string("");
    book.coverBmpPath = obj["coverBmpPath"] | std::string("");
    store.recentBooks.push_back(book);
  }

  LOG_DBG("RBS", "Recent books loaded from file (%d entries)", store.getCount());
  return true;
}
