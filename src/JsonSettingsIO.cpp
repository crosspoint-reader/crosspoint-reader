#include "JsonSettingsIO.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cctype>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "OpdsServerStore.h"
#include "ReadingStats.h"
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
      settings.statusBarLowerProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::HIDE_TITLE;
      settings.statusBarBattery = 0;
      break;
    case CrossPointSettings::NO_PROGRESS:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarLowerProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::BOOK_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarLowerProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::ONLY_BOOK_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarLowerProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::HIDE_TITLE;
      settings.statusBarBattery = 0;
      break;
    case CrossPointSettings::CHAPTER_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 1;
      settings.statusBarProgressBar = CrossPointSettings::CHAPTER_PROGRESS;
      settings.statusBarLowerProgressBar = CrossPointSettings::CHAPTER_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::FULL:
    default:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 1;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarLowerProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
  }

  settings.statusBarUpperProgressBar = CrossPointSettings::HIDE_PROGRESS;
  settings.statusBarUpperProgressBarThickness = CrossPointSettings::PROGRESS_BAR_NORMAL;
  settings.statusBarLowerProgressBarThickness = settings.statusBarProgressBarThickness;
  settings.statusBarItemsPosition = CrossPointSettings::STATUS_BAR_ITEMS_BOTTOM;
}

// ---- CrossPointState ----

bool JsonSettingsIO::saveState(const CrossPointState& s, const char* path) {
  JsonDocument doc;
  doc["openEpubPath"] = s.openEpubPath;
  doc["lastSleepImage"] = s.lastSleepImage;
  doc["readerActivityLoadCount"] = s.readerActivityLoadCount;
  doc["lastSleepFromReader"] = s.lastSleepFromReader;
  doc["recentBooksGridView"] = s.recentBooksGridView;
  doc["showBootScreen"] = s.showBootScreen;
  // Information about a pending KOReader sync session
  JsonObject sync = doc["koReaderSyncSession"].to<JsonObject>();
  sync["active"] = s.koReaderSyncSession.active;
  sync["epubPath"] = s.koReaderSyncSession.epubPath;
  sync["spineIndex"] = s.koReaderSyncSession.spineIndex;
  sync["page"] = s.koReaderSyncSession.page;
  sync["totalPagesInSpine"] = s.koReaderSyncSession.totalPagesInSpine;
  sync["paragraphIndex"] = s.koReaderSyncSession.paragraphIndex;
  sync["hasParagraphIndex"] = s.koReaderSyncSession.hasParagraphIndex;
  sync["xhtmlSeekHint"] = s.koReaderSyncSession.xhtmlSeekHint;
  sync["intent"] = static_cast<uint8_t>(s.koReaderSyncSession.intent);
  sync["outcome"] = static_cast<uint8_t>(s.koReaderSyncSession.outcome);
  sync["resultSpineIndex"] = s.koReaderSyncSession.resultSpineIndex;
  sync["resultPage"] = s.koReaderSyncSession.resultPage;
  sync["resultParagraphIndex"] = s.koReaderSyncSession.resultParagraphIndex;
  sync["resultHasParagraphIndex"] = s.koReaderSyncSession.resultHasParagraphIndex;
  sync["resultListItemIndex"] = s.koReaderSyncSession.resultListItemIndex;
  sync["resultHasListItemIndex"] = s.koReaderSyncSession.resultHasListItemIndex;
  sync["exitToHomeAfterSync"] = s.koReaderSyncSession.exitToHomeAfterSync;
  sync["autoPullEpubPath"] = s.koReaderSyncSession.autoPullEpubPath;
  // Information about a pending bookmark jump
  JsonObject jump = doc["pendingBookmarkJump"].to<JsonObject>();
  jump["active"] = s.pendingBookmarkJump.active;
  jump["bookPath"] = s.pendingBookmarkJump.bookPath;
  jump["spineIndex"] = s.pendingBookmarkJump.spineIndex;
  jump["pageNumber"] = s.pendingBookmarkJump.pageNumber;

  if (doc.overflowed()) {
    LOG_ERR("CPS", "JSON document overflowed while building state");
    return false;
  }

  FsFile file;
  if (!Storage.openFileForWrite("CPS", path, file)) {
    return false;
  }

  const size_t expected = measureJson(doc);
  const size_t written = serializeJson(doc, file);
  file.flush();
  if (!file.close()) {
    return false;
  }
  return written == expected;
}

bool JsonSettingsIO::loadState(CrossPointState& s, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("CPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  s.openEpubPath = doc["openEpubPath"] | std::string("");
  s.lastSleepImage = doc["lastSleepImage"] | SIZE_MAX;
  s.readerActivityLoadCount = doc["readerActivityLoadCount"] | (uint8_t)0;
  s.lastSleepFromReader = doc["lastSleepFromReader"] | false;
  s.recentBooksGridView = doc["recentBooksGridView"] | false;
  s.showBootScreen = doc["showBootScreen"] | true;
  JsonObject sync = doc["koReaderSyncSession"].as<JsonObject>();
  s.koReaderSyncSession.active = sync["active"] | false;
  s.koReaderSyncSession.epubPath = sync["epubPath"] | std::string("");
  s.koReaderSyncSession.spineIndex = sync["spineIndex"] | 0;
  s.koReaderSyncSession.page = sync["page"] | 0;
  s.koReaderSyncSession.totalPagesInSpine = sync["totalPagesInSpine"] | 0;
  s.koReaderSyncSession.paragraphIndex = sync["paragraphIndex"] | (uint16_t)0;
  s.koReaderSyncSession.hasParagraphIndex = sync["hasParagraphIndex"] | false;
  s.koReaderSyncSession.xhtmlSeekHint = sync["xhtmlSeekHint"] | (uint32_t)0;
  s.koReaderSyncSession.intent =
      static_cast<KOReaderSyncIntentState>(sync["intent"] | static_cast<uint8_t>(KOReaderSyncIntentState::COMPARE));
  s.koReaderSyncSession.outcome =
      static_cast<KOReaderSyncOutcomeState>(sync["outcome"] | static_cast<uint8_t>(KOReaderSyncOutcomeState::NONE));
  s.koReaderSyncSession.resultSpineIndex = sync["resultSpineIndex"] | 0;
  s.koReaderSyncSession.resultPage = sync["resultPage"] | 0;
  s.koReaderSyncSession.resultParagraphIndex = sync["resultParagraphIndex"] | (uint16_t)0;
  s.koReaderSyncSession.resultHasParagraphIndex = sync["resultHasParagraphIndex"] | false;
  s.koReaderSyncSession.resultListItemIndex = sync["resultListItemIndex"] | (uint16_t)0;
  s.koReaderSyncSession.resultHasListItemIndex = sync["resultHasListItemIndex"] | false;
  s.koReaderSyncSession.exitToHomeAfterSync = sync["exitToHomeAfterSync"] | false;
  s.koReaderSyncSession.autoPullEpubPath = sync["autoPullEpubPath"] | std::string("");
  if (s.koReaderSyncSession.autoPullEpubPath.empty() && (sync["autoPullOnOpen"] | false)) {
    LOG_DBG("CPS", "Legacy autoPullOnOpen state found without epubPath - ignoring");
  }

  JsonObject jump = doc["pendingBookmarkJump"].as<JsonObject>();
  s.pendingBookmarkJump.active = jump["active"] | false;
  s.pendingBookmarkJump.bookPath = jump["bookPath"] | std::string("");
  s.pendingBookmarkJump.spineIndex = jump["spineIndex"] | (uint16_t)0;
  s.pendingBookmarkJump.pageNumber = jump["pageNumber"] | (uint16_t)0;
  return true;
}

// ---- CrossPointSettings ----

bool JsonSettingsIO::saveSettings(const CrossPointSettings& s, const char* path) {
  JsonDocument doc;
  const auto settings = getSettingsList();

  for (const auto& info : settings) {
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

  // Font family uses a DynamicEnumCtx in SettingsList (no valuePtr) so the generic
  // loop above skips it. Save manually.
  doc["fontFamily"] = s.fontFamily;
  if (s.sdFontFamilyName[0] != '\0') {
    doc["sdFontFamilyName"] = s.sdFontFamilyName;
  }
  // TXT/MD font family also uses DynamicEnumCtx (no valuePtr); save manually.
  doc["txtFontFamily"] = s.txtFontFamily;
  if (s.txtSdFontFamilyName[0] != '\0') {
    doc["txtSdFontFamilyName"] = s.txtSdFontFamilyName;
  }
  doc["moveFinishedBooksToCompleted"] = s.moveFinishedBooksToCompleted;
  doc["removeFinishedBooksFromRecents"] = s.removeFinishedBooksFromRecents;

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
    s.statusBar = clamp(doc["statusBar"] | s.statusBar, CrossPointSettings::STATUS_BAR_MODE_COUNT, s.statusBar);
    applyLegacyStatusBarSettings(s);
    if (needsResave) *needsResave = true;
  }

  auto migrateMissingStatusSetting = [&doc, &needsResave, &clamp](const char* newKey, uint8_t& value,
                                                                  const char* legacyKey, uint8_t defaultValue,
                                                                  uint8_t count) {
    if (!doc[newKey].isNull()) {
      return;
    }
    value = clamp(doc[legacyKey] | defaultValue, count, defaultValue);
    if (needsResave) *needsResave = true;
  };

  migrateMissingStatusSetting("statusBarUpperProgressBar", s.statusBarUpperProgressBar, "statusBarUpperProgressBar",
                              CrossPointSettings::HIDE_PROGRESS, CrossPointSettings::STATUS_BAR_PROGRESS_BAR_COUNT);
  migrateMissingStatusSetting("statusBarUpperProgressBarThickness", s.statusBarUpperProgressBarThickness,
                              "statusBarUpperProgressBarThickness", CrossPointSettings::PROGRESS_BAR_NORMAL,
                              CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT);
  migrateMissingStatusSetting("statusBarLowerProgressBar", s.statusBarLowerProgressBar, "statusBarProgressBar",
                              s.statusBarProgressBar, CrossPointSettings::STATUS_BAR_PROGRESS_BAR_COUNT);
  migrateMissingStatusSetting("statusBarLowerProgressBarThickness", s.statusBarLowerProgressBarThickness,
                              "statusBarProgressBarThickness", s.statusBarProgressBarThickness,
                              CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT);
  migrateMissingStatusSetting("statusBarItemsPosition", s.statusBarItemsPosition, "statusBarItemsPosition",
                              CrossPointSettings::STATUS_BAR_ITEMS_BOTTOM,
                              CrossPointSettings::STATUS_BAR_ITEMS_POSITION_COUNT);
  const auto settings = getSettingsList();

  for (const auto& info : settings) {
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

  // Font family uses a DynamicEnumCtx in SettingsList (no valuePtr) so the generic
  // loop above skips it. Load manually.
  s.fontFamily = clamp(doc["fontFamily"] | (uint8_t)CrossPointSettings::BOOKERLY,
                       CrossPointSettings::BUILTIN_FONT_COUNT, CrossPointSettings::BOOKERLY);
  const char* sfn = doc["sdFontFamilyName"] | "";
  strncpy(s.sdFontFamilyName, sfn, sizeof(s.sdFontFamilyName) - 1);
  s.sdFontFamilyName[sizeof(s.sdFontFamilyName) - 1] = '\0';
  // TXT/MD font family is dynamic too; load manually.
  s.txtFontFamily = clamp(doc["txtFontFamily"] | (uint8_t)CrossPointSettings::NOTOSANS,
                          CrossPointSettings::BUILTIN_FONT_COUNT, CrossPointSettings::NOTOSANS);
  const char* txtSfn = doc["txtSdFontFamilyName"] | "";
  strncpy(s.txtSdFontFamilyName, txtSfn, sizeof(s.txtSdFontFamilyName) - 1);
  s.txtSdFontFamilyName[sizeof(s.txtSdFontFamilyName) - 1] = '\0';
  s.moveFinishedBooksToCompleted = doc["moveFinishedBooksToCompleted"] | (uint8_t)0;
  s.removeFinishedBooksFromRecents = doc["removeFinishedBooksFromRecents"] | (uint8_t)0;

  const uint8_t quickResumeBeforeNormalize = s.quickResumeSleepScreen;
  CrossPointSettings::normalizeDependentSettings(s);
  if (s.quickResumeSleepScreen != quickResumeBeforeNormalize && needsResave) *needsResave = true;

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
  doc["sendMetadata"] = store.getSendMetadata();

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
  store.sendMetadata = doc["sendMetadata"] | false;

  LOG_DBG("KRS", "Loaded KOReader credentials for user: %s", store.username.c_str());
  return true;
}

// ---- WifiCredentialStore ----

bool JsonSettingsIO::saveWifi(const WifiCredentialStore& store, const char* path) {
  JsonDocument doc;
  doc["lastConnectedSsid"] = store.getLastConnectedSsid();
  doc["lastKnownMacAddress"] = store.getLastKnownMacAddress();

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

  const auto isValidDashedMac = [](const std::string& value) -> bool {
    if (value.empty()) {
      return true;
    }
    if (value.size() != 17) {
      return false;
    }
    for (size_t i = 0; i < value.size(); i++) {
      if (i == 2 || i == 5 || i == 8 || i == 11 || i == 14) {
        if (value[i] != '-') {
          return false;
        }
      } else if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
        return false;
      }
    }
    return true;
  };

  store.lastKnownMacAddress = doc["lastKnownMacAddress"] | std::string("");
  if (!isValidDashedMac(store.lastKnownMacAddress)) {
    store.lastKnownMacAddress.clear();
    if (needsResave) {
      *needsResave = true;
    }
  }

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
    obj["series"] = book.series;
    obj["coverBmpPath"] = book.coverBmpPath;
    obj["embeddedStyleOverride"] = book.embeddedStyleOverride;
    obj["imageRenderingOverride"] = book.imageRenderingOverride;
    obj["fontFamilyOverride"] = book.fontFamilyOverride;
    if (!book.sdFontFamilyOverride.empty()) {
      obj["sdFontFamilyOverride"] = book.sdFontFamilyOverride;
    }
    obj["fontSizeOverride"] = book.fontSizeOverride;
    obj["bionicReadingOverride"] = book.bionicReadingOverride;
    obj["paragraphAlignmentOverride"] = book.paragraphAlignmentOverride;
    obj["textAntiAliasingOverride"] = book.textAntiAliasingOverride;
    obj["hyphenationOverride"] = book.hyphenationOverride;
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
  auto clampInt8 = [](int value, int minValue, int maxValue, int8_t fallback) -> int8_t {
    if (value < minValue || value > maxValue) {
      return fallback;
    }
    return static_cast<int8_t>(value);
  };

  for (JsonObject obj : arr) {
    if (store.getCount() >= 10) break;
    RecentBook book;
    book.path = obj["path"] | std::string("");
    book.title = obj["title"] | std::string("");
    book.author = obj["author"] | std::string("");
    book.series = obj["series"] | std::string("");
    book.coverBmpPath = obj["coverBmpPath"] | std::string("");
    book.embeddedStyleOverride = clampInt8(obj["embeddedStyleOverride"] | -1, -1, 1, -1);
    book.imageRenderingOverride = clampInt8(obj["imageRenderingOverride"] | -1, -1, 2, -1);
    book.fontFamilyOverride =
        clampInt8(obj["fontFamilyOverride"] | -1, -1, CrossPointSettings::FONT_FAMILY_COUNT - 1, -1);
    book.sdFontFamilyOverride = obj["sdFontFamilyOverride"] | std::string("");
    if (!book.sdFontFamilyOverride.empty()) {
      // Keep built-in and SD font overrides mutually exclusive.
      book.fontFamilyOverride = -1;
    }
    book.fontSizeOverride = clampInt8(obj["fontSizeOverride"] | -1, -1, CrossPointSettings::FONT_SIZE_COUNT - 1, -1);
    book.bionicReadingOverride = clampInt8(obj["bionicReadingOverride"] | -1, -1, 1, -1);
    book.paragraphAlignmentOverride =
        clampInt8(obj["paragraphAlignmentOverride"] | -1, -1, CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT - 1, -1);
    book.textAntiAliasingOverride = clampInt8(obj["textAntiAliasingOverride"] | -1, -1, 1, -1);
    book.hyphenationOverride = clampInt8(obj["hyphenationOverride"] | -1, -1, 1, -1);
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

// ---- ReadingStatsStore ----

bool JsonSettingsIO::saveReadingStats(const ReadingStatsStore& store, const char* path) {
  JsonDocument doc;
  doc["totalSeconds"] = store.getGlobalTotalSeconds();
  doc["totalSessions"] = store.getGlobalTotalSessions();
  doc["totalPagesTurned"] = store.getGlobalTotalPagesTurned();

  // Day buckets are serialised as a flat array of [dayIndex, seconds] pairs
  // to keep the file compact when many days are populated. The C++ side
  // already keeps days sorted, so we preserve that on disk too.
  auto writeDays = [](JsonArray out, const std::vector<DayBucket>& days) {
    for (const auto& d : days) {
      JsonArray pair = out.add<JsonArray>();
      pair.add(d.dayIndex);
      pair.add(d.seconds);
    }
  };

  writeDays(doc["globalDays"].to<JsonArray>(), store.getGlobalDays());

  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : store.getBooks()) {
    JsonObject obj = arr.add<JsonObject>();
    obj["docId"] = book.docId;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["totalSeconds"] = book.totalSeconds;
    obj["pagesTurned"] = book.pagesTurned;
    obj["sessions"] = book.sessions;
    obj["firstReadEpoch"] = static_cast<int64_t>(book.firstReadEpoch);
    obj["lastReadEpoch"] = static_cast<int64_t>(book.lastReadEpoch);
    obj["progress"] = book.progress;
    obj["finishedCount"] = book.finishedCount;
    obj["lastFinishedEpoch"] = static_cast<int64_t>(book.lastFinishedEpoch);
    // Derived for backwards-compatibility with consumers (web dashboard,
    // older firmware) that still read the bool field.
    obj["finished"] = book.finishedCount > 0;
    writeDays(obj["days"].to<JsonArray>(), book.days);
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadReadingStats(ReadingStatsStore& store, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("RST", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.books.clear();
  store.globalDays.clear();
  store.globalTotalSeconds = doc["totalSeconds"] | (uint32_t)0;
  store.globalTotalSessions = doc["totalSessions"] | (uint32_t)0;
  store.globalTotalPagesTurned = doc["totalPagesTurned"] | (uint32_t)0;

  // Reads [dayIndex, seconds] pairs into a DayBucket vector, dropping
  // malformed entries. We don't re-sort because saver writes in order; the
  // result of accidentally hand-edited unsorted input is just degraded
  // streak/sparkline accuracy, not a crash.
  auto readDays = [](JsonArray in, std::vector<DayBucket>& out) {
    for (JsonArray pair : in) {
      if (pair.size() < 2) continue;
      DayBucket b;
      b.dayIndex = pair[0] | (uint16_t)0;
      b.seconds = pair[1] | (uint32_t)0;
      if (b.dayIndex == 0 || b.seconds == 0) continue;
      out.push_back(b);
    }
  };

  readDays(doc["globalDays"].as<JsonArray>(), store.globalDays);

  JsonArray arr = doc["books"].as<JsonArray>();
  for (JsonObject obj : arr) {
    BookReadingStats book;
    book.docId = obj["docId"] | std::string("");
    if (book.docId.empty()) continue;  // skip corrupt entries
    book.title = obj["title"] | std::string("");
    book.author = obj["author"] | std::string("");
    book.totalSeconds = obj["totalSeconds"] | (uint32_t)0;
    book.pagesTurned = obj["pagesTurned"] | (uint32_t)0;
    book.sessions = obj["sessions"] | (uint32_t)0;
    book.firstReadEpoch = static_cast<time_t>(obj["firstReadEpoch"] | (int64_t)0);
    book.lastReadEpoch = static_cast<time_t>(obj["lastReadEpoch"] | (int64_t)0);
    book.progress = obj["progress"] | (uint8_t)0;
    // finishedCount is the canonical field. Old files that only have the
    // bool "finished" land here as 1 so the per-book screen still shows the
    // book as having been finished at least once.
    if (!obj["finishedCount"].isNull()) {
      book.finishedCount = obj["finishedCount"] | (uint16_t)0;
    } else if (obj["finished"] | false) {
      book.finishedCount = 1;
    }
    book.lastFinishedEpoch = static_cast<time_t>(obj["lastFinishedEpoch"] | (int64_t)0);
    readDays(obj["days"].as<JsonArray>(), book.days);
    store.books.push_back(std::move(book));
  }

  LOG_DBG("RST", "Reading stats loaded (%zu books, %u s total)", store.books.size(), store.globalTotalSeconds);
  return true;
}
