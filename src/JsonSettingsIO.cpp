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
#include "RecentBooksStore.h"
#include "SettingsList.h"
#include "TouchGestures.h"
#include "TouchUi.h"
#include "WifiCredentialStore.h"
#include "activities/home/HomeMenu.h"
#include "activities/home/LibraryTab.h"
#include "util/UrlUtils.h"

namespace {
// Upper bound on any stored credential we will decode from JSON. A WPA passphrase
// tops out at 63 characters (or a 64-hex-char PSK); server passwords get the same
// generous cap. Anything longer is corrupt or hand-edited, and decoding it would
// cost contiguous heap we can't spare.
constexpr size_t MAX_PASSWORD_LENGTH = 64;
}  // namespace

// ---- CrossPointState ----

bool JsonSettingsIO::saveState(const CrossPointState& s, const char* path) {
  JsonDocument doc;
  doc["openEpubPath"] = s.openEpubPath;
  doc["lastSleepImage"] = s.lastSleepImage;
  doc["readerActivityLoadCount"] = s.readerActivityLoadCount;
  doc["lastSleepFromReader"] = s.lastSleepFromReader;
  doc["recentBooksView"] = s.recentBooksView;
  doc["libraryTab"] = s.libraryTab;
  doc["addedBooksView"] = s.addedBooksView;
  doc["authorsView"] = s.authorsView;
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
  sync["paragraphIndexBefore"] = s.koReaderSyncSession.paragraphIndexBefore;
  sync["visibleOffsetAtPage"] = s.koReaderSyncSession.visibleOffsetAtPage;
  sync["visibleOffsetAtNextPage"] = s.koReaderSyncSession.visibleOffsetAtNextPage;
  sync["hasVisibleOffset"] = s.koReaderSyncSession.hasVisibleOffset;
  sync["intent"] = static_cast<uint8_t>(s.koReaderSyncSession.intent);
  sync["outcome"] = static_cast<uint8_t>(s.koReaderSyncSession.outcome);
  sync["resultSpineIndex"] = s.koReaderSyncSession.resultSpineIndex;
  sync["resultPage"] = s.koReaderSyncSession.resultPage;
  sync["resultParagraphIndex"] = s.koReaderSyncSession.resultParagraphIndex;
  sync["resultHasParagraphIndex"] = s.koReaderSyncSession.resultHasParagraphIndex;
  sync["resultListItemIndex"] = s.koReaderSyncSession.resultListItemIndex;
  sync["resultHasListItemIndex"] = s.koReaderSyncSession.resultHasListItemIndex;
  sync["resultVisibleOffset"] = s.koReaderSyncSession.resultVisibleOffset;
  sync["resultHasVisibleOffset"] = s.koReaderSyncSession.resultHasVisibleOffset;
  sync["postAction"] = static_cast<uint8_t>(s.koReaderSyncSession.postAction);
  sync["postActionTarget"] = s.koReaderSyncSession.postActionTarget;
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
  s.readerActivityLoadCount = doc["readerActivityLoadCount"].as<uint8_t>();
  s.lastSleepFromReader = doc["lastSleepFromReader"].as<bool>();
  // Before Recent Books became a Browse Files list it had a grid/list switch: the grid is now
  // the Covers view, the list the Details view. Only a stored false chose the list.
  const JsonVariantConst legacyGridView = doc["recentBooksGridView"];
  const bool legacyListView = legacyGridView.is<bool>() && !legacyGridView.as<bool>();
  const uint8_t fallbackView =
      legacyListView ? CrossPointSettings::BROWSER_VIEW_DETAILS : CrossPointSettings::BROWSER_VIEW_COVERS;
  s.recentBooksView = doc["recentBooksView"] | fallbackView;
  if (s.recentBooksView >= CrossPointSettings::FILE_BROWSER_VIEW_COUNT) s.recentBooksView = fallbackView;
  s.libraryTab = static_cast<uint8_t>(libraryTabFrom(doc["libraryTab"].as<uint8_t>()));
  s.addedBooksView = doc["addedBooksView"] | uint8_t{CrossPointSettings::BROWSER_VIEW_COVERS};
  if (s.addedBooksView >= CrossPointSettings::FILE_BROWSER_VIEW_COUNT) {
    s.addedBooksView = CrossPointSettings::BROWSER_VIEW_COVERS;
  }
  s.authorsView = doc["authorsView"] | uint8_t{CrossPointSettings::BROWSER_VIEW_DETAILS};
  if (s.authorsView >= CrossPointSettings::FILE_BROWSER_VIEW_COUNT) {
    s.authorsView = CrossPointSettings::BROWSER_VIEW_DETAILS;
  }
  s.showBootScreen = doc["showBootScreen"] | true;
  JsonObject sync = doc["koReaderSyncSession"].as<JsonObject>();
  s.koReaderSyncSession.active = sync["active"].as<bool>();
  s.koReaderSyncSession.epubPath = sync["epubPath"] | std::string("");
  s.koReaderSyncSession.spineIndex = sync["spineIndex"].as<int>();
  s.koReaderSyncSession.page = sync["page"].as<int>();
  s.koReaderSyncSession.totalPagesInSpine = sync["totalPagesInSpine"].as<int>();
  s.koReaderSyncSession.paragraphIndex = sync["paragraphIndex"].as<uint16_t>();
  s.koReaderSyncSession.hasParagraphIndex = sync["hasParagraphIndex"].as<bool>();
  s.koReaderSyncSession.paragraphIndexBefore = sync["paragraphIndexBefore"].as<uint16_t>();
  s.koReaderSyncSession.visibleOffsetAtPage = sync["visibleOffsetAtPage"].as<uint32_t>();
  s.koReaderSyncSession.visibleOffsetAtNextPage = sync["visibleOffsetAtNextPage"] | (uint32_t)UINT32_MAX;
  s.koReaderSyncSession.hasVisibleOffset = sync["hasVisibleOffset"].as<bool>();
  s.koReaderSyncSession.intent =
      static_cast<KOReaderSyncIntentState>(sync["intent"] | static_cast<uint8_t>(KOReaderSyncIntentState::COMPARE));
  s.koReaderSyncSession.outcome =
      static_cast<KOReaderSyncOutcomeState>(sync["outcome"] | static_cast<uint8_t>(KOReaderSyncOutcomeState::NONE));
  s.koReaderSyncSession.resultSpineIndex = sync["resultSpineIndex"].as<int>();
  s.koReaderSyncSession.resultPage = sync["resultPage"].as<int>();
  s.koReaderSyncSession.resultParagraphIndex = sync["resultParagraphIndex"].as<uint16_t>();
  s.koReaderSyncSession.resultHasParagraphIndex = sync["resultHasParagraphIndex"].as<bool>();
  s.koReaderSyncSession.resultListItemIndex = sync["resultListItemIndex"].as<uint16_t>();
  s.koReaderSyncSession.resultHasListItemIndex = sync["resultHasListItemIndex"].as<bool>();
  s.koReaderSyncSession.resultVisibleOffset = sync["resultVisibleOffset"].as<uint32_t>();
  s.koReaderSyncSession.resultHasVisibleOffset = sync["resultHasVisibleOffset"].as<bool>();
  s.koReaderSyncSession.postAction =
      static_cast<KOReaderSyncPostAction>(sync["postAction"] | static_cast<uint8_t>(KOReaderSyncPostAction::Reader));
  s.koReaderSyncSession.postActionTarget = sync["postActionTarget"] | std::string("");
  s.koReaderSyncSession.autoPullEpubPath = sync["autoPullEpubPath"] | std::string("");
  if (s.koReaderSyncSession.autoPullEpubPath.empty() && sync["autoPullOnOpen"].as<bool>()) {
    LOG_DBG("CPS", "Legacy autoPullOnOpen state found without epubPath - ignoring");
  }

  JsonObject jump = doc["pendingBookmarkJump"].as<JsonObject>();
  s.pendingBookmarkJump.active = jump["active"].as<bool>();
  s.pendingBookmarkJump.bookPath = jump["bookPath"] | std::string("");
  s.pendingBookmarkJump.spineIndex = jump["spineIndex"].as<uint16_t>();
  s.pendingBookmarkJump.pageNumber = jump["pageNumber"].as<uint16_t>();
  return true;
}

// ---- CrossPointSettings ----

bool JsonSettingsIO::saveSettings(const CrossPointSettings& s, const char* path) {
  JsonDocument doc;
  forEachSetting([&](const SettingInfo& info) {
    if (!info.key) return;
    // persistPtr covers rows whose UI type carries no field pointer of its own (slider ACTIONs).
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    const auto field = info.valuePtr ? info.valuePtr : info.persistPtr;
    if (!field && !info.stringOffset) return;

    if (info.stringOffset) {
      const char* strPtr = reinterpret_cast<const char*>(&s) + info.stringOffset;
      if (info.obfuscated) {
        doc[std::string(info.key) + "_obf"] = obfuscation::obfuscateToBase64(strPtr);
      } else {
        doc[info.key] = strPtr;
      }
    } else {
      doc[info.key] = s.*field;
    }
  });

  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  doc["frontButtonBack"] = s.frontButtonBack;
  doc["frontButtonConfirm"] = s.frontButtonConfirm;
  doc["frontButtonLeft"] = s.frontButtonLeft;
  doc["frontButtonRight"] = s.frontButtonRight;

  // Font family uses a DynamicEnumCtx in SettingsList (no valuePtr) so the generic
  // loop above skips it. Save manually.
  doc["fontFamily"] = s.fontFamily;
  // Stamps which generation of gesture defaults this file was written against.
  // See CrossPointSettings::GESTURE_DEFAULTS_VERSION.
  doc["gestureDefaultsV"] = s.gestureDefaultsVersion;
  // Which meaning the fontSize / txtFontSize values below carry (point sizes since version 2).
  // See CrossPointSettings::FONT_SIZE_ORDER_VERSION.
  doc["fontSizeOrderV"] = CrossPointSettings::FONT_SIZE_ORDER_VERSION;
  // Same for the frontlight switch, which is a DynamicToggle because the live
  // hardware rather than this field is the authority for "is the light on".
  // Saved unconditionally, not behind the board's capability: a settings file
  // carried to a board with no light must come back with the preference intact.
  doc["frontlightOn"] = s.frontlightOn;
  if (s.dictionaryName[0] != '\0') {
    doc["dictionaryName"] = s.dictionaryName;
  }
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
  doc["syncFinishedBookToKOReader"] = s.syncFinishedBookToKOReader;
  // Left out while unconfigured, so the default keeps following the UI language.
  if (s.keyboardLayouts != 0) doc["keyboardLayouts"] = s.keyboardLayouts;

  // Write nothing rather than a file cut short: a truncated settings.json reads back as a parse
  // error and every setting returns to its default. The document reports a refused allocation
  // itself; the String does not (ArduinoJson's String writer counts bytes it failed to append),
  // so its length is checked against what the document should serialise to.
  if (doc.overflowed()) {
    LOG_ERR("CPS", "Settings not saved: JSON document out of memory");
    return false;
  }
  const size_t expected = measureJson(doc);
  String json;
  serializeJson(doc, json);
  if (json.length() != expected) {
    LOG_ERR("CPS", "Settings not saved: %u of %u bytes of JSON fitted", static_cast<unsigned>(json.length()),
            static_cast<unsigned>(expected));
    return false;
  }
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

  // Gesture keys written against an older generation of the shipped defaults are
  // ignored below, so the compiled defaults win. Absent (0) counts as older,
  // which is exactly the case for every file written before the stamp existed.
  const bool staleGestureDefaults =
      doc["gestureDefaultsV"].as<int>() < static_cast<int>(CrossPointSettings::GESTURE_DEFAULTS_VERSION);
  // Cannot match on a board with no digitiser: the loop below walks the FILTERED
  // settings list, which carries no gesture rows there, so no gesture key ever
  // reaches this. Gated rather than left to be evaluated and always fail, because
  // the BINDINGS table it walks is 400 bytes of .rodata that nothing else in such
  // a build refers to.
  const auto isGestureKey = [](const char* key) {
#if CP_TOUCH_UI
    if (key == nullptr) return false;
    for (const auto& binding : TouchGestures::BINDINGS) {
      if (std::strcmp(key, binding.key) == 0) return true;
    }
#else
    (void)key;
#endif
    return false;
  };

  forEachSetting([&](const SettingInfo& info) {
    if (!info.key) return;
    // See the matching comment in saveSettings.
    const auto field = info.valuePtr ? info.valuePtr : info.persistPtr;
    if (!field && !info.stringOffset) return;

    if (info.stringOffset) {
      const char* strPtr = reinterpret_cast<const char*>(&s) + info.stringOffset;
      const std::string fieldDefault = strPtr;  // current buffer = struct-initializer default
      std::string val;
      if (info.obfuscated) {
        bool ok = false;
        bool tooLong = false;
        // The value is about to be truncated into a stringMaxLen buffer anyway, so
        // anything longer is already garbage — reject it before it costs us heap.
        val = obfuscation::deobfuscateFromBase64(doc[std::string(info.key) + "_obf"] | "",
                                                 info.stringMaxLen ? info.stringMaxLen - 1 : 0, &ok, &tooLong);
        if (tooLong) {
          LOG_ERR("CPS", "Oversized obfuscated value for key '%s'", info.key);
          if (needsResave) *needsResave = true;
        }
        if (!ok || val.empty()) {
          val = doc[info.key] | fieldDefault;
          if (val != fieldDefault && needsResave) *needsResave = true;
        }
      } else {
        val = doc[info.key] | fieldDefault;
      }
      char* destPtr = reinterpret_cast<char*>(&s) + info.stringOffset;
      if (info.stringMaxLen == 0) {
        LOG_ERR("CPS", "Misconfigured SettingInfo: stringMaxLen is 0 for key '%s'", info.key);
        destPtr[0] = '\0';
        if (needsResave) *needsResave = true;
        return;
      }
      strncpy(destPtr, val.c_str(), info.stringMaxLen - 1);
      destPtr[info.stringMaxLen - 1] = '\0';
    } else {
      const uint8_t fieldDefault = s.*field;  // struct-initializer default, read before we overwrite it
      // A gesture key written against an older generation of the defaults is
      // ignored, not loaded: a stored BTN_DEFAULT is indistinguishable from a
      // key that predates the default, so honouring it would pin every device
      // that has ever saved its settings to whatever the defaults were then.
      // See CrossPointSettings::GESTURE_DEFAULTS_VERSION.
      if (staleGestureDefaults && isGestureKey(info.key)) {
        s.*field = fieldDefault;
        if (needsResave) *needsResave = true;
        return;
      }
      uint8_t v = doc[info.key] | fieldDefault;
      if (info.persistPtr) {
        // A slider's range is its own; anything past it falls back rather than driving the
        // hardware (or the sleep timer) somewhere the UI would never have offered.
        v = v <= info.persistMax ? v : fieldDefault;
      } else if (info.type == SettingType::ENUM) {
        // getEnumOptionCount(), not enumValues.size(): a row that carries enumLabels instead of
        // translated StrIds (the font-size rows) has no enumValues at all, and bounding on that
        // rejected every value including the stored one.
        v = clamp(v, info.getEnumOptionCount(), fieldDefault);
      } else if (info.type == SettingType::TOGGLE) {
        v = clamp(v, (uint8_t)2, fieldDefault);
      } else if (info.type == SettingType::VALUE) {
        if (v < info.valueRange.min)
          v = info.valueRange.min;
        else if (v > info.valueRange.max)
          v = info.valueRange.max;
      }
      s.*field = v;
    }
  });

  // Browse Files and Recent Books had a home entry each before they became the Library's tabs. The
  // Library shows where either did; the old keys go with the next save.
  if (doc["showLibraryOnHome"].isNull()) {
    s.showLibraryOnHome =
        libraryOnHomeFromLegacy(doc["showBrowseFilesOnHome"] | uint8_t{1}, doc["showRecentBooksOnHome"] | uint8_t{1});
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
  // Frontlight switch: dynamic in SettingsList, so it needs loading manually too.
  s.frontlightOn = doc["frontlightOn"].as<int>() != 0 ? 1 : 0;
  const char* dictName = doc["dictionaryName"] | "";
  strncpy(s.dictionaryName, dictName, sizeof(s.dictionaryName) - 1);
  s.dictionaryName[sizeof(s.dictionaryName) - 1] = '\0';
  const char* sfn = doc["sdFontFamilyName"] | "";
  strncpy(s.sdFontFamilyName, sfn, sizeof(s.sdFontFamilyName) - 1);
  s.sdFontFamilyName[sizeof(s.sdFontFamilyName) - 1] = '\0';
  // TXT/MD font family is dynamic too; load manually.
  s.txtFontFamily = clamp(doc["txtFontFamily"] | (uint8_t)CrossPointSettings::NOTOSANS,
                          CrossPointSettings::BUILTIN_FONT_COUNT, CrossPointSettings::NOTOSANS);
  const char* txtSfn = doc["txtSdFontFamilyName"] | "";
  strncpy(s.txtSdFontFamilyName, txtSfn, sizeof(s.txtSdFontFamilyName) - 1);
  s.txtSdFontFamilyName[sizeof(s.txtSdFontFamilyName) - 1] = '\0';
  s.moveFinishedBooksToCompleted = doc["moveFinishedBooksToCompleted"].as<uint8_t>();
  s.removeFinishedBooksFromRecents = doc["removeFinishedBooksFromRecents"].as<uint8_t>();
  s.syncFinishedBookToKOReader = doc["syncFinishedBookToKOReader"].as<uint8_t>();
  // Unknown bits (a file from a build with more layouts) are dropped by keyboard_layouts::enabled().
  s.keyboardLayouts = doc["keyboardLayouts"].as<uint16_t>();

  const uint8_t quickResumeBeforeNormalize = s.quickResumeSleepScreen;
  CrossPointSettings::normalizeDependentSettings(s);
  // Reader sizes are point sizes since FONT_SIZE_ORDER_VERSION 2; older files hold FONT_SIZE
  // values. Read from the document rather than from the fields the loop above filled: that loop
  // bounds by persistMax and cannot tell an absent key from a stored one, and an absent key must
  // mean the default, not whatever point size a stale enum value happens to name. Idempotent
  // across boots: an unconverted file keeps its old stamp until it is rewritten.
  {
    const auto fileVersion = static_cast<uint8_t>(doc["fontSizeOrderV"].as<int>());
    const auto loadPointSize = [&](const char* key) -> uint8_t {
      const uint8_t pt = CrossPointSettings::fontPointSizeFromStored(doc[key] | -1, fileVersion);
      return pt != 0 ? pt : CrossPointSettings::DEFAULT_FONT_POINT_SIZE;
    };
    s.fontPointSize = loadPointSize("fontSize");
    s.txtFontPointSize = loadPointSize("txtFontSize");
    if (fileVersion < CrossPointSettings::FONT_SIZE_ORDER_VERSION && needsResave) *needsResave = true;
  }

  // The retired TIMEZONE enum, read straight out of the document because it is no longer a row
  // in the settings list -- it is migration input, not a setting anyone can still choose. It
  // feeds timezones::activeIndex(), which turns it into a clockTimezone index the first time
  // this firmware boots on a device that predates that field. Reading it through a list row
  // instead would mean carrying a dead, web-visible "Timezone" number alongside the real one.
  //
  // Dropping out of the list also means saveToFile() stops writing it, so the key disappears
  // from the file once the migration has been resaved. That is the intent: it has one job.
  {
    const auto legacy = static_cast<uint8_t>(doc["timeZone"] | (uint8_t)CrossPointSettings::TZ_UTC);
    s.timeZone = legacy < CrossPointSettings::TZ_LEGACY_COUNT ? legacy : (uint8_t)CrossPointSettings::TZ_UTC;
  }

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
  doc["syncBehavior"] = static_cast<uint8_t>(store.getSyncBehavior());
  doc["syncOnWake"] = store.getSyncOnWake();
  doc["syncOnSleep"] = store.getSyncOnSleep();
  doc["pushIntervalPages"] = store.getPushIntervalPages();
  doc["showSyncIndicator"] = store.getShowSyncIndicator();

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
  bool tooLong = false;
  store.password = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", MAX_PASSWORD_LENGTH, &ok, &tooLong);
  if (tooLong) {
    LOG_ERR("KRS", "Discarding oversized password for user: %s", store.username.c_str());
    if (needsResave) *needsResave = true;
  }
  if (!ok || store.password.empty()) {
    store.password = doc["password"] | std::string("");
    if (store.password.size() > MAX_PASSWORD_LENGTH) {
      LOG_ERR("KRS", "Discarding oversized legacy password for user: %s", store.username.c_str());
      store.password.clear();
      if (needsResave) *needsResave = true;
    } else if (!store.password.empty() && needsResave) {
      *needsResave = true;
    }
  }
  // Repair a scheme typo saved by an older build, so the settings screen shows the
  // same URL getBaseUrl() will connect to.
  store.serverUrl = UrlUtils::repairSchemeSeparator(doc["serverUrl"] | std::string(""));
  uint8_t method = doc["matchMethod"].as<uint8_t>();
  store.matchMethod = static_cast<DocumentMatchMethod>(method);
  store.sendMetadata = doc["sendMetadata"].as<bool>();

  // A file written before this key existed belongs to someone already using the chooser.
  // Default them to ASK_EVERY_TIME rather than the SMART the member initialiser carries, so
  // an update never silently changes how their sync resolves; only fresh installs get SMART.
  const JsonVariantConst behaviorValue = doc["syncBehavior"];
  if (behaviorValue.isNull()) {
    store.setSyncBehavior(KOReaderSyncBehavior::ASK_EVERY_TIME);
    if (needsResave) *needsResave = true;
  } else {
    store.setSyncBehavior(static_cast<KOReaderSyncBehavior>(behaviorValue.as<uint8_t>()));
  }

  store.syncOnWake = doc["syncOnWake"].as<bool>();
  store.syncOnSleep = doc["syncOnSleep"].as<bool>();
  const uint16_t pushPages = doc["pushIntervalPages"].as<uint16_t>();
  store.setPushIntervalPages(pushPages);
  if (needsResave && store.pushIntervalPages != pushPages) *needsResave = true;
  store.showSyncIndicator = doc["showSyncIndicator"] | true;

  LOG_DBG("KRS", "Loaded KOReader credentials for user: %s", store.username.c_str());
  return true;
}

// ---- WifiCredentialStore ----

bool JsonSettingsIO::saveWifi(const WifiCredentialStore& store, const char* path) {
  JsonDocument doc;
  doc["lastConnectedSsid"] = store.getLastConnectedSsid();
  // The key the passwords below are obfuscated with, whatever the store last recorded: a save that
  // did not pass through the WiFi screen (the web settings page in hotspot mode) must still say
  // which key it used. loadWifi() relies on it to read a file another reader wrote.
  doc["lastKnownMacAddress"] = obfuscation::deviceKeyId();

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
    if (cred.apCount > 0) obj["aps"] = cred.apCount;
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

  // Passwords are XORed with the key of the reader that wrote them, and lastKnownMacAddress names
  // that reader: saveWifi() records its own key, and older firmware re-saved the file under its own
  // key whenever it recorded a new one. A file from another reader (a card moved over, a copied
  // wifi.json) is therefore read with the writer's key and re-saved under ours. Read with ours it
  // gave passwords of the right length and the wrong bytes, which the router rejects as a 4-way
  // handshake timeout - indistinguishable from a weak signal on the device.
  uint8_t ownKey[6];
  uint8_t writerKey[6];
  const bool fromOtherReader = obfuscation::parseKeyId(obfuscation::deviceKeyId(), ownKey) &&
                               obfuscation::parseKeyId(store.lastKnownMacAddress, writerKey) &&
                               std::memcmp(ownKey, writerKey, sizeof(ownKey)) != 0;
  if (fromOtherReader) {
    LOG_INF("WCS", "wifi.json was written by reader %s; re-keying its passwords for this one",
            store.lastKnownMacAddress.c_str());
    store.lastKnownMacAddress = obfuscation::deviceKeyId();
    if (needsResave) *needsResave = true;
  }

  store.credentials.clear();
  JsonArray arr = doc["credentials"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (store.credentials.size() >= store.MAX_NETWORKS) break;
    WifiCredential cred;
    cred.ssid = obj["ssid"] | std::string("");
    bool ok = false;
    bool tooLong = false;
    cred.password = obfuscation::deobfuscateFromBase64(obj["password_obf"] | "", MAX_PASSWORD_LENGTH, &ok, &tooLong,
                                                       fromOtherReader ? writerKey : nullptr);
    if (tooLong) {
      LOG_ERR("WCS", "Discarding oversized password for %s", cred.ssid.c_str());
      if (needsResave) *needsResave = true;
    }
    if (!ok || cred.password.empty()) {
      cred.password = obj["password"] | std::string("");
      if (cred.password.size() > MAX_PASSWORD_LENGTH) {
        LOG_ERR("WCS", "Discarding oversized legacy password for %s", cred.ssid.c_str());
        cred.password.clear();
        if (needsResave) *needsResave = true;
      } else if (!cred.password.empty() && needsResave) {
        *needsResave = true;
      }
    }
    const std::string bssidHex = obj["bssid"] | std::string("");
    const int channel = obj["channel"].as<int>();
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
    const int aps = obj["aps"].as<int>();  // 0 when absent: files from before the count
    cred.apCount = static_cast<uint8_t>(aps < 0 ? 0 : (aps > 255 ? 255 : aps));
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
        cred.cacheTimestamp = obj["ts"].as<unsigned>();
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
  // Which numbering each book's fontSizeOverride is in. Its own stamp rather than the settings
  // file's: the two files are written independently, so one can be migrated while the other is
  // still on the old numbering. See CrossPointSettings::FONT_SIZE_ORDER_VERSION.
  doc["fontSizeOrderV"] = CrossPointSettings::FONT_SIZE_ORDER_VERSION;
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : store.getBooks()) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["series"] = book.series;
    obj["coverBmpPath"] = book.coverBmpPath;
    obj["metadataStamp"] = book.metadataStamp;
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
    obj["fontSizeNormalizationOverride"] = book.fontSizeNormalizationOverride;
    obj["guideDotsOverride"] = book.guideDotsOverride;
    obj["inlineFootnotePreviewsOverride"] = book.inlineFootnotePreviewsOverride;
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
  // Which meaning the per-book size overrides in this file carry (point sizes since version 2).
  // Absent (0) is every file written before the sizes were put in pixel order.
  const auto fontSizeOrderV = static_cast<uint8_t>(doc["fontSizeOrderV"].as<int>());
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
    // Absent in files written before it existed: 0, so an entry whose book has
    // a sidecar is refreshed once.
    book.metadataStamp = obj["metadataStamp"].as<unsigned>();
    book.embeddedStyleOverride = clampInt8(obj["embeddedStyleOverride"] | -1, -1, 1, -1);
    book.imageRenderingOverride = clampInt8(obj["imageRenderingOverride"] | -1, -1, 2, -1);
    book.fontFamilyOverride =
        clampInt8(obj["fontFamilyOverride"] | -1, -1, CrossPointSettings::FONT_FAMILY_COUNT - 1, -1);
    book.sdFontFamilyOverride = obj["sdFontFamilyOverride"] | std::string("");
    if (!book.sdFontFamilyOverride.empty()) {
      // Keep built-in and SD font overrides mutually exclusive.
      book.fontFamilyOverride = -1;
    }
    // -1 means "follow the default". Anything else is a size in this file's meaning; one that does
    // not convert to a usable point size goes back to the default rather than to a guess.
    const uint8_t overridePt =
        CrossPointSettings::fontPointSizeFromStored(obj["fontSizeOverride"] | -1, fontSizeOrderV);
    book.fontSizeOverride = overridePt != 0 ? static_cast<int8_t>(overridePt) : -1;
    book.bionicReadingOverride = clampInt8(obj["bionicReadingOverride"] | -1, -1, 1, -1);
    book.paragraphAlignmentOverride =
        clampInt8(obj["paragraphAlignmentOverride"] | -1, -1, CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT - 1, -1);
    book.textAntiAliasingOverride = clampInt8(obj["textAntiAliasingOverride"] | -1, -1, 1, -1);
    book.hyphenationOverride = clampInt8(obj["hyphenationOverride"] | -1, -1, 1, -1);
    book.fontSizeNormalizationOverride = clampInt8(obj["fontSizeNormalizationOverride"] | -1, -1, 1, -1);
    book.guideDotsOverride = clampInt8(obj["guideDotsOverride"] | -1, -1, 1, -1);
    book.inlineFootnotePreviewsOverride = clampInt8(obj["inlineFootnotePreviewsOverride"] | -1, -1, 1, -1);
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
    bool tooLong = false;
    server.password = obfuscation::deobfuscateFromBase64(obj["password_obf"] | "", MAX_PASSWORD_LENGTH, &ok, &tooLong);
    if (tooLong) {
      LOG_ERR("OPS", "Discarding oversized password for server: %s", server.name.c_str());
      if (needsResave) *needsResave = true;
    }
    if (!ok || server.password.empty()) {
      server.password = obj["password"] | std::string("");
      if (server.password.size() > MAX_PASSWORD_LENGTH) {
        LOG_ERR("OPS", "Discarding oversized legacy password for server: %s", server.name.c_str());
        server.password.clear();
        if (needsResave) *needsResave = true;
      } else if (!server.password.empty() && needsResave) {
        *needsResave = true;
      }
    }
    store.servers.push_back(std::move(server));
  }

  LOG_DBG("OPS", "Loaded %zu OPDS servers from file", store.servers.size());
  return true;
}
