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

// Convert legacy settings.
void applyLegacyStatusBarSettings(CrossPointSettings& settings) {
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

// ---- CrossPointState ----

bool JsonSettingsIO::saveState(const CrossPointState& s, const char* path) {
  JsonDocument doc;
  doc["openEpubPath"] = s.openEpubPath;
  doc["lastSleepImage"] = s.lastSleepImage;
  doc["readerActivityLoadCount"] = s.readerActivityLoadCount;
  doc["lastSleepFromReader"] = s.lastSleepFromReader;

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
  return true;
}

// ---- CrossPointSettings ----

bool JsonSettingsIO::saveSettings(const CrossPointSettings& s, const char* path) {
  JsonDocument doc;

  doc["sleepScreen"] = s.sleepScreen;
  doc["sleepScreenCoverMode"] = s.sleepScreenCoverMode;
  doc["sleepScreenCoverFilter"] = s.sleepScreenCoverFilter;
  doc["statusBar"] = s.statusBar;
  doc["extraParagraphSpacing"] = s.extraParagraphSpacing;
  doc["textAntiAliasing"] = s.textAntiAliasing;
  doc["shortPwrBtn"] = s.shortPwrBtn;
  doc["orientation"] = s.orientation;
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
  doc["longPressButtonBehavior"] = s.longPressButtonBehavior;
  doc["hyphenationEnabled"] = s.hyphenationEnabled;
  doc["uiTheme"] = s.uiTheme;
  doc["fadingFix"] = s.fadingFix;
  doc["embeddedStyle"] = s.embeddedStyle;
  doc["statusBarChapterPageCount"] = s.statusBarChapterPageCount;
  doc["statusBarBookProgressPercentage"] = s.statusBarBookProgressPercentage;
  doc["statusBarProgressBar"] = s.statusBarProgressBar;
  doc["statusBarTitle"] = s.statusBarTitle;
  doc["statusBarBattery"] = s.statusBarBattery;
  doc["statusBarProgressBarThickness"] = s.statusBarProgressBarThickness;

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
  s.sleepScreenCoverMode =
      clamp(doc["sleepScreenCoverMode"] | (uint8_t)S::FIT, S::SLEEP_SCREEN_COVER_MODE_COUNT, S::FIT);
  s.sleepScreenCoverFilter =
      clamp(doc["sleepScreenCoverFilter"] | (uint8_t)S::NO_FILTER, S::SLEEP_SCREEN_COVER_FILTER_COUNT, S::NO_FILTER);
  s.statusBar = clamp(doc["statusBar"] | (uint8_t)S::FULL, S::STATUS_BAR_MODE_COUNT, S::FULL);
  s.extraParagraphSpacing = doc["extraParagraphSpacing"] | (uint8_t)1;
  s.textAntiAliasing = doc["textAntiAliasing"] | (uint8_t)1;
  s.shortPwrBtn = clamp(doc["shortPwrBtn"] | (uint8_t)S::IGNORE, S::SHORT_PWRBTN_COUNT, S::IGNORE);
  s.orientation = clamp(doc["orientation"] | (uint8_t)S::PORTRAIT, S::ORIENTATION_COUNT, S::PORTRAIT);
  s.sideButtonLayout =
      clamp(doc["sideButtonLayout"] | (uint8_t)S::PREV_NEXT, S::SIDE_BUTTON_LAYOUT_COUNT, S::PREV_NEXT);
  s.frontButtonBack =
      clamp(doc["frontButtonBack"] | (uint8_t)S::FRONT_HW_BACK, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_BACK);
  s.frontButtonConfirm = clamp(doc["frontButtonConfirm"] | (uint8_t)S::FRONT_HW_CONFIRM, S::FRONT_BUTTON_HARDWARE_COUNT,
                               S::FRONT_HW_CONFIRM);
  s.frontButtonLeft =
      clamp(doc["frontButtonLeft"] | (uint8_t)S::FRONT_HW_LEFT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_LEFT);
  s.frontButtonRight =
      clamp(doc["frontButtonRight"] | (uint8_t)S::FRONT_HW_RIGHT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_RIGHT);
  CrossPointSettings::validateFrontButtonMapping(s);
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
  s.longPressButtonBehavior =
      clamp(doc["longPressButtonBehavior"] | (uint8_t)S::OFF, S::CHAPTER_SKIP, S::ORIENTATION_CHANGE);
  s.hyphenationEnabled = doc["hyphenationEnabled"] | (uint8_t)0;
  s.uiTheme = doc["uiTheme"] | (uint8_t)S::LYRA;
  s.fadingFix = doc["fadingFix"] | (uint8_t)0;
  s.embeddedStyle = doc["embeddedStyle"] | (uint8_t)1;

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
  LOG_DBG("CPS", "Settings loaded from file");

  if (doc.containsKey("statusBarChapterPageCount")) {
    s.statusBarChapterPageCount = doc["statusBarChapterPageCount"];
    s.statusBarBookProgressPercentage = doc["statusBarBookProgressPercentage"];
    s.statusBarProgressBar = doc["statusBarProgressBar"];
    s.statusBarTitle = doc["statusBarTitle"];
    s.statusBarBattery = doc["statusBarBattery"];
  } else {
    applyLegacyStatusBarSettings(s);
  }

  s.statusBarProgressBarThickness = doc["statusBarProgressBarThickness"] | (uint8_t)S::PROGRESS_BAR_NORMAL;

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
