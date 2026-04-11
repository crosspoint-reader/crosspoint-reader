#include "JsonSettingsIO.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "RecentBooksStore.h"
#include "SettingsList.h"
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
  s.lastSleepImage = doc["lastSleepImage"] | (uint8_t)UINT8_MAX;
  s.readerActivityLoadCount = doc["readerActivityLoadCount"] | (uint8_t)0;
  s.lastSleepFromReader = doc["lastSleepFromReader"] | false;
  return true;
}

// ---- CrossPointSettings ----

bool JsonSettingsIO::saveSettings(const CrossPointSettings& s, const char* path) {
  JsonDocument doc;

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) continue;

    if (info.stringOffset) {
      const char* strPtr = (const char*)&s + info.stringOffset;
      if (info.obfuscated) {
        doc[std::string(info.key) + "_obf"] = obfuscation::obfuscateToBase64(strPtr);
      } else {
        doc[info.key] = strPtr;
      }
    } else {
      doc[info.key] = s.*(info.valuePtr);
    }
  }

  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  doc["frontButtonBack"] = s.frontButtonBack;
  doc["frontButtonConfirm"] = s.frontButtonConfirm;
  doc["frontButtonLeft"] = s.frontButtonLeft;
  doc["frontButtonRight"] = s.frontButtonRight;
  // Tilt page turn (X3 only, not in SettingsList)
  doc["tiltPageTurn"] = s.tiltPageTurn;

  // Direction-specific settings (nested objects)
  auto saveDirection = [](JsonObject obj, const DirectionSettings& ds) {
    obj["fontFamily"] = ds.fontFamily;
    if (ds.sdFontFamilyName[0] != '\0') {
      obj["sdFontFamilyName"] = ds.sdFontFamilyName;
    }
    obj["fontSize"] = ds.fontSize;
    obj["lineSpacing"] = ds.lineSpacing;
    obj["charSpacing"] = ds.charSpacing;
    obj["paragraphAlignment"] = ds.paragraphAlignment;
    obj["extraParagraphSpacing"] = ds.extraParagraphSpacing;
    obj["hyphenationEnabled"] = ds.hyphenationEnabled;
    obj["screenMargin"] = ds.screenMargin;
    obj["firstLineIndent"] = ds.firstLineIndent;
    obj["textAntiAliasing"] = ds.textAntiAliasing;
  };
  saveDirection(doc["horizontal"].to<JsonObject>(), s.horizontal);
  saveDirection(doc["vertical"].to<JsonObject>(), s.vertical);

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

  auto clamp = [](uint8_t val, uint8_t maxVal, uint8_t def) -> uint8_t { return val < maxVal ? val : def; };

  // Legacy migration: if statusBarChapterPageCount is absent this is a pre-refactor settings file.
  // Populate s with migrated values now so the generic loop below picks them up as defaults and clamps them.
  if (doc["statusBarChapterPageCount"].isNull()) {
    applyLegacyStatusBarSettings(s);
  }

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) continue;

    if (info.stringOffset) {
      const char* strPtr = (const char*)&s + info.stringOffset;
      const std::string fieldDefault = strPtr;  // current buffer = struct-initializer default
      std::string val;
      if (info.obfuscated) {
        bool ok = false;
        val = obfuscation::deobfuscateFromBase64(doc[std::string(info.key) + "_obf"] | "", &ok);
        if (!ok || val.empty()) {
          val = doc[info.key] | fieldDefault;
          if (val != fieldDefault && needsResave) *needsResave = true;
        }
      } else {
        val = doc[info.key] | fieldDefault;
      }
      char* destPtr = (char*)&s + info.stringOffset;
      if (info.stringMaxLen == 0) {
        LOG_ERR("CPS", "Misconfigured SettingInfo: stringMaxLen is 0 for key '%s'", info.key);
        destPtr[0] = '\0';
        if (needsResave) *needsResave = true;
        continue;
      }
      strncpy(destPtr, val.c_str(), info.stringMaxLen - 1);
      destPtr[info.stringMaxLen - 1] = '\0';
    } else {
      const uint8_t fieldDefault = s.*(info.valuePtr);  // struct-initializer default, read before we overwrite it
      uint8_t v = doc[info.key] | fieldDefault;
      if (info.type == SettingType::ENUM) {
        v = clamp(v, (uint8_t)info.enumValues.size(), fieldDefault);
      } else if (info.type == SettingType::TOGGLE) {
        v = clamp(v, (uint8_t)2, fieldDefault);
      } else if (info.type == SettingType::VALUE) {
        if (v < info.valueRange.min)
          v = info.valueRange.min;
        else if (v > info.valueRange.max)
          v = info.valueRange.max;
      }
      s.*(info.valuePtr) = v;
    }
  }

  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  using S = CrossPointSettings;
  s.frontButtonBack =
      clamp(doc["frontButtonBack"] | (uint8_t)S::FRONT_HW_BACK, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_BACK);
  s.frontButtonConfirm = clamp(doc["frontButtonConfirm"] | (uint8_t)S::FRONT_HW_CONFIRM, S::FRONT_BUTTON_HARDWARE_COUNT,
                               S::FRONT_HW_CONFIRM);
  s.frontButtonLeft =
      clamp(doc["frontButtonLeft"] | (uint8_t)S::FRONT_HW_LEFT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_LEFT);
  s.frontButtonRight =
      clamp(doc["frontButtonRight"] | (uint8_t)S::FRONT_HW_RIGHT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_RIGHT);
  CrossPointSettings::validateFrontButtonMapping(s);
  // Tilt page turn (X3 only, not in SettingsList)
  s.tiltPageTurn = clamp(doc["tiltPageTurn"] | (uint8_t)0, 2, 0);

  // Load direction-specific settings (nested objects)
  auto loadDirection = [](JsonObject obj, DirectionSettings& ds) -> bool {
    if (obj.isNull()) return false;
    ds.fontFamily = obj["fontFamily"] | ds.fontFamily;
    if (ds.fontFamily >= CrossPointSettings::FONT_FAMILY_COUNT) ds.fontFamily = 0;
    const char* sfn = obj["sdFontFamilyName"] | "";
    strncpy(ds.sdFontFamilyName, sfn, sizeof(ds.sdFontFamilyName) - 1);
    ds.sdFontFamilyName[sizeof(ds.sdFontFamilyName) - 1] = '\0';
    ds.fontSize = obj["fontSize"] | ds.fontSize;
    if (ds.fontSize >= CrossPointSettings::FONT_SIZE_COUNT) ds.fontSize = 1;
    ds.lineSpacing = obj["lineSpacing"] | ds.lineSpacing;
    if (ds.lineSpacing < CrossPointSettings::LINE_SPACING_MIN) ds.lineSpacing = CrossPointSettings::LINE_SPACING_MIN;
    if (ds.lineSpacing > CrossPointSettings::LINE_SPACING_MAX) ds.lineSpacing = CrossPointSettings::LINE_SPACING_MAX;
    ds.charSpacing = obj["charSpacing"] | ds.charSpacing;
    if (ds.charSpacing > 50) ds.charSpacing = 0;
    ds.paragraphAlignment = obj["paragraphAlignment"] | ds.paragraphAlignment;
    if (ds.paragraphAlignment >= CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT) ds.paragraphAlignment = 0;
    ds.extraParagraphSpacing = obj["extraParagraphSpacing"] | ds.extraParagraphSpacing;
    ds.hyphenationEnabled = obj["hyphenationEnabled"] | ds.hyphenationEnabled;
    ds.screenMargin = obj["screenMargin"] | ds.screenMargin;
    if (ds.screenMargin < 5) ds.screenMargin = 5;
    if (ds.screenMargin > 40) ds.screenMargin = 40;
    ds.firstLineIndent = obj["firstLineIndent"] | ds.firstLineIndent;
    ds.textAntiAliasing = obj["textAntiAliasing"] | ds.textAntiAliasing;
    return true;
  };

  if (!loadDirection(doc["horizontal"].as<JsonObject>(), s.horizontal)) {
    // Migration from flat format: copy old values to both directions
    auto migrateBoth = [&](const char* key, uint8_t DirectionSettings::* field) {
      uint8_t val = doc[key] | s.horizontal.*field;
      s.horizontal.*field = val;
      s.vertical.*field = val;
    };
    migrateBoth("extraParagraphSpacing", &DirectionSettings::extraParagraphSpacing);
    migrateBoth("textAntiAliasing", &DirectionSettings::textAntiAliasing);
    migrateBoth("paragraphAlignment", &DirectionSettings::paragraphAlignment);
    migrateBoth("hyphenationEnabled", &DirectionSettings::hyphenationEnabled);
    migrateBoth("screenMargin", &DirectionSettings::screenMargin);
    migrateBoth("firstLineIndent", &DirectionSettings::firstLineIndent);
    migrateBoth("fontSize", &DirectionSettings::fontSize);
    // fontFamily
    uint8_t ff = doc["fontFamily"] | (uint8_t)0;
    if (ff >= CrossPointSettings::BUILTIN_FONT_COUNT) ff = 0;
    s.horizontal.fontFamily = ff;
    s.vertical.fontFamily = ff;
    // sdFontFamilyName
    const char* sfn = doc["sdFontFamilyName"] | "";
    strncpy(s.horizontal.sdFontFamilyName, sfn, sizeof(s.horizontal.sdFontFamilyName) - 1);
    s.horizontal.sdFontFamilyName[sizeof(s.horizontal.sdFontFamilyName) - 1] = '\0';
    strncpy(s.vertical.sdFontFamilyName, sfn, sizeof(s.vertical.sdFontFamilyName) - 1);
    s.vertical.sdFontFamilyName[sizeof(s.vertical.sdFontFamilyName) - 1] = '\0';
    // lineSpacing: use legacy split fields if present, else single lineSpacing
    if (!doc["lineSpacingHorizontal"].isNull()) {
      s.horizontal.lineSpacing = doc["lineSpacingHorizontal"] | s.horizontal.lineSpacing;
    } else if (!doc["lineSpacing"].isNull()) {
      uint8_t ls = doc["lineSpacing"] | CrossPointSettings::LINE_SPACING_DEFAULT;
      s.horizontal.lineSpacing = ls;
    }
    if (!doc["lineSpacingVertical"].isNull()) {
      s.vertical.lineSpacing = doc["lineSpacingVertical"] | s.vertical.lineSpacing;
    } else if (!doc["lineSpacing"].isNull()) {
      uint8_t ls = doc["lineSpacing"] | CrossPointSettings::LINE_SPACING_DEFAULT;
      s.vertical.lineSpacing = ls;
    }
    // verticalCharSpacing → vertical.charSpacing only
    s.vertical.charSpacing = doc["verticalCharSpacing"] | s.vertical.charSpacing;
    // Clamp migrated values to valid ranges
    auto clampDs = [](DirectionSettings& d) {
      if (d.fontFamily >= CrossPointSettings::FONT_FAMILY_COUNT) d.fontFamily = 0;
      if (d.fontSize >= CrossPointSettings::FONT_SIZE_COUNT) d.fontSize = 1;
      if (d.lineSpacing < CrossPointSettings::LINE_SPACING_MIN) d.lineSpacing = CrossPointSettings::LINE_SPACING_MIN;
      if (d.lineSpacing > CrossPointSettings::LINE_SPACING_MAX) d.lineSpacing = CrossPointSettings::LINE_SPACING_MAX;
      if (d.charSpacing > 50) d.charSpacing = 0;
      if (d.paragraphAlignment >= CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT) d.paragraphAlignment = 0;
      if (d.screenMargin < 5) d.screenMargin = 5;
      if (d.screenMargin > 40) d.screenMargin = 40;
    };
    clampDs(s.horizontal);
    clampDs(s.vertical);
    if (needsResave) *needsResave = true;
  } else {
    loadDirection(doc["vertical"].as<JsonObject>(), s.vertical);
  }

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
