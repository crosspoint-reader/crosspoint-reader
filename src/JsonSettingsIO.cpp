#include "JsonSettingsIO.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "OpdsServerStore.h"
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
  JsonArray recentArr = doc["recentSleepImages"].to<JsonArray>();
  for (int i = 0; i < CrossPointState::SLEEP_RECENT_COUNT; i++) recentArr.add(s.recentSleepImages[i]);
  doc["recentSleepPos"] = s.recentSleepPos;
  doc["recentSleepFill"] = s.recentSleepFill;
  doc["readerActivityLoadCount"] = s.readerActivityLoadCount;
  doc["lastSleepFromReader"] = s.lastSleepFromReader;
  doc["showBootScreen"] = s.showBootScreen;

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
  memset(s.recentSleepImages, 0, sizeof(s.recentSleepImages));
  JsonArrayConst recentArr = doc["recentSleepImages"];
  const int actualCount = recentArr.isNull() ? 0
                                             : std::min(static_cast<int>(recentArr.size()),
                                                        static_cast<int>(CrossPointState::SLEEP_RECENT_COUNT));
  for (int i = 0; i < actualCount; i++) s.recentSleepImages[i] = recentArr[i] | static_cast<uint16_t>(0);
  s.recentSleepPos = doc["recentSleepPos"] | static_cast<uint8_t>(0);
  if (s.recentSleepPos >= CrossPointState::SLEEP_RECENT_COUNT)
    s.recentSleepPos = actualCount > 0 ? s.recentSleepPos % CrossPointState::SLEEP_RECENT_COUNT : 0;
  s.recentSleepFill = doc["recentSleepFill"] | static_cast<uint8_t>(0);
  s.recentSleepFill = static_cast<uint8_t>(std::min(static_cast<int>(s.recentSleepFill), actualCount));
  // Migrate legacy single-image field from old state.json (pre-recency-buffer).
  // Only seeds the buffer if the new buffer is empty (fresh migration, not a resave).
  if (s.recentSleepFill == 0 && !doc["lastSleepImage"].isNull()) {
    const uint8_t legacy = doc["lastSleepImage"] | static_cast<uint8_t>(UINT8_MAX);
    if (legacy != UINT8_MAX) s.pushRecentSleep(static_cast<uint16_t>(legacy));
  }
  s.readerActivityLoadCount = doc["readerActivityLoadCount"] | static_cast<uint8_t>(0);
  s.lastSleepFromReader = doc["lastSleepFromReader"] | false;
  s.showBootScreen = doc["showBootScreen"] | true;
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
  // Font family — uses dynamic getter/setter in SettingsList so the generic loop skips it.
  doc["fontFamily"] = s.fontFamily;
  // SD card font family name — not in SettingsList, save manually
  if (s.sdFontFamilyName[0] != '\0') {
    doc["sdFontFamilyName"] = s.sdFontFamilyName;
  }

  // Language -- managed by LanguageSelectActivity, not in SettingsList.
  // Stored as ISO code string ("EN", "DE", ...) for stability across enum reorders.
  doc["language"] = (s.language < getLanguageCount()) ? LANGUAGE_CODES[s.language] : "EN";

  // Language -- managed by LanguageSelectActivity, not in SettingsList.
  // Stored as ISO code string ("EN", "DE", ...) for stability across enum reorders.
  doc["language"] = (s.language < getLanguageCount()) ? LANGUAGE_CODES[s.language] : "EN";

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

  // Font family — uses dynamic getter/setter in SettingsList so the generic loop skips it.
  s.fontFamily = clamp(doc["fontFamily"] | (uint8_t)0, CrossPointSettings::BUILTIN_FONT_COUNT, 0);
  // SD card font family name — not in SettingsList, load manually
  const char* sfn = doc["sdFontFamilyName"] | "";
  strncpy(s.sdFontFamilyName, sfn, sizeof(s.sdFontFamilyName) - 1);
  s.sdFontFamilyName[sizeof(s.sdFontFamilyName) - 1] = '\0';

  // Language -- stored as code string for stability across enum reorders.
  if (doc["language"].is<const char*>()) {
    s.language = static_cast<uint8_t>(I18n::languageFromCode(doc["language"].as<const char*>()));
  }

  LOG_DBG("CPS", "Settings loaded from file");

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
    bool hasHint = cred.channel != 0;
    for (int i = 0; i < 6 && !hasHint; i++) {
      if (cred.bssid[i] != 0) hasHint = true;
    }
    if (hasHint) {
      char bssidHex[13];
      snprintf(bssidHex, sizeof(bssidHex), "%02x%02x%02x%02x%02x%02x", cred.bssid[0], cred.bssid[1], cred.bssid[2],
               cred.bssid[3], cred.bssid[4], cred.bssid[5]);
      obj["bssid"] = bssidHex;
      obj["channel"] = cred.channel;
    }
    if (cred.ip[0] != 0) {
      char buf[16];
      snprintf(buf, sizeof(buf), "%d.%d.%d.%d", cred.ip[0], cred.ip[1], cred.ip[2], cred.ip[3]);
      obj["ip"] = buf;
      snprintf(buf, sizeof(buf), "%d.%d.%d.%d", cred.gateway[0], cred.gateway[1], cred.gateway[2], cred.gateway[3]);
      obj["gw"] = buf;
      snprintf(buf, sizeof(buf), "%d.%d.%d.%d", cred.mask[0], cred.mask[1], cred.mask[2], cred.mask[3]);
      obj["mask"] = buf;
      snprintf(buf, sizeof(buf), "%d.%d.%d.%d", cred.dns[0], cred.dns[1], cred.dns[2], cred.dns[3]);
      obj["dns"] = buf;
      obj["ts"] = cred.cacheTimestamp;
    }
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
    const std::string bssidHex = obj["bssid"] | std::string("");
    const int channel = obj["channel"] | 0;
    if (bssidHex.size() == 12 && channel > 0 && channel <= 14) {
      bool parseOk = true;
      uint8_t parsed[6] = {0};
      for (int i = 0; i < 6 && parseOk; i++) {
        unsigned int byte = 0;
        if (sscanf(bssidHex.c_str() + i * 2, "%2x", &byte) != 1) {
          parseOk = false;
        } else {
          parsed[i] = static_cast<uint8_t>(byte);
        }
      }
      if (parseOk) {
        std::memcpy(cred.bssid, parsed, 6);
        cred.channel = static_cast<uint8_t>(channel);
      }
    }
    const auto parseQuad = [](const std::string& s, uint8_t out[4]) -> bool {
      unsigned int a = 0, b = 0, c = 0, d = 0;
      int consumed = 0;
      // %n stores characters consumed; require it to equal full string length so we
      // reject inputs with trailing garbage like "192.168.1.10xyz".
      if (sscanf(s.c_str(), "%u.%u.%u.%u%n", &a, &b, &c, &d, &consumed) != 4) return false;
      if (consumed < 0 || static_cast<size_t>(consumed) != s.size()) return false;
      if (a > 255 || b > 255 || c > 255 || d > 255) return false;
      out[0] = a;
      out[1] = b;
      out[2] = c;
      out[3] = d;
      return true;
    };
    const std::string ipStr = obj["ip"] | std::string("");
    const std::string gwStr = obj["gw"] | std::string("");
    const std::string maskStr = obj["mask"] | std::string("");
    const std::string dnsStr = obj["dns"] | std::string("");
    if (!ipStr.empty()) {
      uint8_t ip[4], gw[4], mask[4], dns[4];
      if (parseQuad(ipStr, ip) && parseQuad(gwStr, gw) && parseQuad(maskStr, mask) && parseQuad(dnsStr, dns)) {
        std::memcpy(cred.ip, ip, 4);
        std::memcpy(cred.gateway, gw, 4);
        std::memcpy(cred.mask, mask, 4);
        std::memcpy(cred.dns, dns, 4);
        cred.cacheTimestamp = obj["ts"] | 0u;
      }
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

// ---- OpdsServerStore ----
// Follows the same save/load pattern as WifiCredentialStore above.
// Passwords are XOR-obfuscated with the device MAC and base64-encoded ("password_obf" key).

bool JsonSettingsIO::saveOpds(const OpdsServerStore& store, const char* path) {
  JsonDocument doc;

  JsonArray arr = doc["servers"].to<JsonArray>();
  for (const auto& server : store.getServers()) {
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = server.name;
    obj["url"] = server.url;
    obj["username"] = server.username;
    obj["password_obf"] = obfuscation::obfuscateToBase64(server.password);
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadOpds(OpdsServerStore& store, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("OPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.servers.clear();
  JsonArray arr = doc["servers"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (store.servers.size() >= OpdsServerStore::MAX_SERVERS) break;
    OpdsServer server;
    server.name = obj["name"] | std::string("");
    server.url = obj["url"] | std::string("");
    server.username = obj["username"] | std::string("");
    // Try the obfuscated key first; fall back to plaintext "password" for
    // files written before obfuscation was added (or hand-edited JSON).
    bool ok = false;
    server.password = obfuscation::deobfuscateFromBase64(obj["password_obf"] | "", &ok);
    if (!ok || server.password.empty()) {
      server.password = obj["password"] | std::string("");
      if (!server.password.empty() && needsResave) *needsResave = true;
    }
    store.servers.push_back(std::move(server));
  }

  LOG_DBG("OPS", "Loaded %zu OPDS servers from file", store.servers.size());
  return true;
}
