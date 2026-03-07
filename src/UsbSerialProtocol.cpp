#include "UsbSerialProtocol.h"

#if ENABLE_USB_MASS_STORAGE

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>  // for logSerial (the real HWCDC)
#include <ObfuscationUtils.h>
#include <WiFi.h>
#include <mbedtls/base64.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "JsonSettingsIO.h"
#include "RecentBooksStore.h"
#include "SpiBusMutex.h"
#include "WifiCredentialStore.h"
#include "activities/todo/TodoPlannerStorage.h"
#include "core/features/FeatureModules.h"
#include "esp_ota_ops.h"
#include "util/BookProgressDataStore.h"
#include "util/DateUtils.h"
#include "util/PathUtils.h"

namespace {

constexpr uint8_t CROSSPOINT_PROTOCOL_VERSION = 1;

// Sized to fit the largest incoming command: ota_chunk with a 4096-char base64 payload
// {"cmd":"ota_chunk","arg":{"data":"<4096 chars>"}}  ≈ 4130 bytes + null
static char s_lineBuf[4200];
static int s_lineLen = 0;

// File upload state machine ───────────────────────────────────────────────
static FsFile s_uploadFile;
static bool s_uploadInProgress = false;

// OTA flash state machine ─────────────────────────────────────────────────
static esp_ota_handle_t s_otaHandle = 0;
static const esp_partition_t* s_otaPartition = nullptr;
static bool s_otaInProgress = false;

// Static buffers for base64 I/O (avoids heap allocation on ESP32-C3)
// ─ Download/cover streaming: 576 raw → 768 base64 chars per iteration
static uint8_t s_rawBuf[576];
static uint8_t s_encBuf[780];  // 768 base64 + padding
// ─ File upload decode: upload_chunk ≤ 512 base64 chars → ≤ 384 decoded bytes
static uint8_t s_decodeBuf[400];
// ─ OTA decode: ota_chunk ≤ 4096 base64 chars → ≤ 3072 decoded bytes
static uint8_t s_otaDecodeBuf[3200];

// ── Response helpers ───────────────────────────────────────────────────────

static void sendOk() { logSerial.print(F("{\"ok\":true}\n")); }

static void sendError(const char* msg) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = msg;
  serializeJson(doc, logSerial);
  logSerial.write('\n');
}

// ── Settings serialization ─────────────────────────────────────────────────
// Mirrors JsonSettingsIO::saveSettings but populates a JsonDocument directly.

static void buildSettingsDoc(JsonDocument& doc) {
  const CrossPointSettings& s = SETTINGS;
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
}

// ── Base64 streaming helper ────────────────────────────────────────────────
// Opens a file and streams it base64-encoded directly to logSerial.
// Caller must have written the JSON prefix (e.g. {"ok":true,"data":"} before calling,
// and must write the closing +"}\n" after.

static bool streamFileBase64(FsFile& file) {
  while (true) {
    size_t bytesRead = 0;
    {
      SpiBusMutex::Guard guard;
      bytesRead = file.read(s_rawBuf, sizeof(s_rawBuf));
    }
    if (bytesRead == 0) break;

    size_t encLen = 0;
    mbedtls_base64_encode(s_encBuf, sizeof(s_encBuf), &encLen, s_rawBuf, bytesRead);
    logSerial.write(s_encBuf, encLen);
  }
  return true;
}

// ── Command handlers ───────────────────────────────────────────────────────

static void handleStatus() {
  JsonDocument resp;
  resp["ok"] = true;
  resp["version"] = CROSSPOINT_VERSION;
  resp["protocolVersion"] = CROSSPOINT_PROTOCOL_VERSION;
  resp["freeHeap"] = (uint32_t)ESP.getFreeHeap();
  resp["uptime"] = millis() / 1000;
  resp["openBook"] = APP_STATE.openEpubPath.c_str();
  serializeJson(resp, logSerial);
  logSerial.write('\n');
}

static void handlePlugins() {
  JsonDocument resp;
  resp["ok"] = true;

  JsonDocument featuresDoc;
  deserializeJson(featuresDoc, core::FeatureModules::getFeatureMapJson());
  resp["plugins"] = featuresDoc.as<JsonObjectConst>();

  serializeJson(resp, logSerial);
  logSerial.write('\n');
}

static void handleOpenBook(const char* path) {
  if (!core::FeatureModules::hasCapability(core::Capability::RemoteOpenBook)) {
    sendError("remote_open_book disabled");
    return;
  }
  if (!path || path[0] == '\0') {
    sendError("missing path");
    return;
  }
  if (!PathUtils::isValidSdPath(String(path))) {
    sendError("invalid path");
    return;
  }
  bool exists = false;
  {
    SpiBusMutex::Guard guard;
    exists = Storage.exists(path);
  }
  if (!exists) {
    sendError("file not found");
    return;
  }
  APP_STATE.pendingOpenPath = path;
  sendOk();
}

static void handleWifiStatus() {
  JsonDocument resp;
  resp["ok"] = true;
  const wl_status_t wifiSt = WiFi.status();
  const bool connected = wifiSt == WL_CONNECTED;
  resp["connected"] = connected;
  if (connected) {
    resp["ssid"] = WiFi.SSID().c_str();
    resp["ip"] = WiFi.localIP().toString().c_str();
    resp["rssi"] = WiFi.RSSI();
  } else {
    const char* stStr = (wifiSt == WL_CONNECT_FAILED)  ? "failed"
                        : (wifiSt == WL_NO_SSID_AVAIL) ? "no_ssid"
                        : (wifiSt == WL_IDLE_STATUS)   ? "connecting"
                                                       : "disconnected";
    resp["status"] = stStr;
  }
  serializeJson(resp, logSerial);
  logSerial.write('\n');
}

static void handleRemoteButton(const char* btn) {
  if (!core::FeatureModules::hasCapability(core::Capability::RemotePageTurn)) {
    sendError("remote_page_turn disabled");
    return;
  }
  int8_t pageTurn = 0;
  if (strcmp(btn, "page_forward") == 0 || strcmp(btn, "next") == 0) {
    pageTurn = 1;
  } else if (strcmp(btn, "page_back") == 0 || strcmp(btn, "prev") == 0 || strcmp(btn, "previous") == 0) {
    pageTurn = -1;
  } else {
    sendError("unknown button; use page_forward or page_back");
    return;
  }
  APP_STATE.pendingPageTurn = pageTurn;
  sendOk();
}

// Android expects: {"ok":true,"files":[{"name":"...","path":"...","dir":false,"size":...,"modified":0}]}
static void handleList(const char* path) {
  if (!PathUtils::isValidSdPath(String(path))) {
    sendError("invalid path");
    return;
  }

  FsFile root;
  {
    SpiBusMutex::Guard guard;
    root = Storage.open(path);
  }

  bool isDir = false;
  if (root) {
    SpiBusMutex::Guard guard;
    isDir = root.isDirectory();
  }

  if (!root || !isDir) {
    if (root) {
      SpiBusMutex::Guard guard;
      root.close();
    }
    sendError("not a directory");
    return;
  }

  logSerial.print(F("{\"ok\":true,\"files\":["));
  bool first = true;

  // Normalize path for constructing full entry paths
  String base(path);
  if (!base.endsWith("/")) base += '/';

  while (true) {
    char name[256] = {0};
    bool entryIsDir = false;
    uint32_t entrySize = 0;
    bool valid = false;

    {
      SpiBusMutex::Guard guard;
      FsFile file = root.openNextFile();
      if (!file) break;
      file.getName(name, sizeof(name));
      entryIsDir = file.isDirectory();
      entrySize = entryIsDir ? 0 : (uint32_t)file.size();
      valid = true;
      file.close();
    }

    if (!valid) break;
    if (name[0] == '.') continue;  // skip hidden entries

    if (!first) logSerial.write(',');
    first = false;

    String entryPath = base + name;
    JsonDocument entry;
    entry["name"] = name;
    entry["path"] = entryPath.c_str();
    entry["dir"] = entryIsDir;
    entry["size"] = entrySize;
    entry["modified"] = (uint32_t)0;
    serializeJson(entry, logSerial);
  }

  {
    SpiBusMutex::Guard guard;
    root.close();
  }
  logSerial.print(F("]}\n"));
}

// Android expects: {"ok":true,"data":"<base64-encoded file>"}
static void handleDownload(const char* path) {
  if (!PathUtils::isValidSdPath(String(path))) {
    sendError("invalid path");
    return;
  }

  FsFile file;
  bool opened = false;
  bool isDir = false;
  {
    SpiBusMutex::Guard guard;
    opened = Storage.openFileForRead("USB", path, file);
    if (opened) isDir = file.isDirectory();
  }

  if (!opened || isDir) {
    if (opened) {
      SpiBusMutex::Guard guard;
      file.close();
    }
    sendError("cannot open file");
    return;
  }

  logSerial.print(F("{\"ok\":true,\"data\":\""));
  streamFileBase64(file);
  {
    SpiBusMutex::Guard guard;
    file.close();
  }
  logSerial.print(F("\"}\n"));
}

// Android sends: {"cmd":"upload_start","arg":{"name":"file.epub","path":"/dir","size":1234}}
static void handleUploadStart(const char* name, const char* dir, uint32_t /*size*/) {
  if (s_uploadInProgress) {
    SpiBusMutex::Guard guard;
    s_uploadFile.close();
    s_uploadInProgress = false;
  }

  String destPath(dir);
  if (!destPath.endsWith("/")) destPath += '/';
  destPath += name;

  if (!PathUtils::isValidSdPath(destPath)) {
    sendError("invalid path");
    return;
  }

  bool opened = false;
  {
    SpiBusMutex::Guard guard;
    opened = Storage.openFileForWrite("USB", destPath.c_str(), s_uploadFile);
  }

  if (!opened) {
    sendError("cannot open file for write");
    return;
  }

  s_uploadInProgress = true;
  sendOk();
}

// Android sends 512-char base64 chunks → decodes to ≤384 bytes
static void handleUploadChunk(const char* b64data) {
  if (!s_uploadInProgress) {
    sendError("no upload in progress");
    return;
  }

  const size_t b64len = strlen(b64data);
  size_t decodedLen = 0;
  const int rc = mbedtls_base64_decode(s_decodeBuf, sizeof(s_decodeBuf), &decodedLen, (const uint8_t*)b64data, b64len);
  if (rc != 0) {
    SpiBusMutex::Guard guard;
    s_uploadFile.close();
    s_uploadInProgress = false;
    sendError("base64 decode error");
    return;
  }

  bool writeOk = false;
  {
    SpiBusMutex::Guard guard;
    writeOk = (s_uploadFile.write(s_decodeBuf, decodedLen) == decodedLen);
  }

  if (!writeOk) {
    SpiBusMutex::Guard guard;
    s_uploadFile.close();
    s_uploadInProgress = false;
    sendError("write failed");
    return;
  }

  sendOk();
}

static void handleUploadDone() {
  if (!s_uploadInProgress) {
    sendError("no upload in progress");
    return;
  }
  {
    SpiBusMutex::Guard guard;
    s_uploadFile.close();
  }
  s_uploadInProgress = false;
  sendOk();
}

// Android sends: {"cmd":"delete","arg":["/path/a","/path/b"]}
static void handleDelete(JsonArrayConst paths) {
  bool anyFailed = false;
  for (JsonVariantConst entry : paths) {
    const char* path = entry.as<const char*>();
    if (!path || !PathUtils::isValidSdPath(String(path))) {
      anyFailed = true;
      continue;
    }
    bool ok = false;
    {
      SpiBusMutex::Guard guard;
      ok = Storage.remove(path);
      if (!ok) ok = Storage.removeDir(path);
    }
    if (!ok) anyFailed = true;
  }
  if (anyFailed) {
    sendError("one or more deletes failed");
  } else {
    sendOk();
  }
}

static void handleMkdir(const char* path) {
  if (!PathUtils::isValidSdPath(String(path))) {
    sendError("invalid path");
    return;
  }
  bool ok = false;
  {
    SpiBusMutex::Guard guard;
    ok = Storage.mkdir(path);
  }
  if (!ok) {
    sendError("mkdir failed");
    return;
  }
  sendOk();
}

// Android sends: {"cmd":"rename","arg":{"from":"/old/path","to":"/new/path"}}
static void handleRename(const char* from, const char* to) {
  if (!PathUtils::isValidSdPath(String(from)) || !PathUtils::isValidSdPath(String(to))) {
    sendError("invalid path");
    return;
  }
  bool ok = false;
  {
    SpiBusMutex::Guard guard;
    ok = Storage.rename(from, to);
  }
  if (!ok) {
    sendError("rename failed");
    return;
  }
  sendOk();
}

// Android sends: {"cmd":"move","arg":{"from":"/src/file.epub","to":"/dst/dir"}}
// "to" is a destination directory; preserve the source filename.
static void handleMove(const char* from, const char* toDir) {
  if (!PathUtils::isValidSdPath(String(from)) || !PathUtils::isValidSdPath(String(toDir))) {
    sendError("invalid path");
    return;
  }
  String fromStr(from);
  const int lastSlash = fromStr.lastIndexOf('/');
  const String filename = (lastSlash >= 0) ? fromStr.substring(lastSlash + 1) : fromStr;

  String dest(toDir);
  if (!dest.endsWith("/")) dest += '/';
  dest += filename;

  bool ok = false;
  {
    SpiBusMutex::Guard guard;
    ok = Storage.rename(from, dest.c_str());
  }
  if (!ok) {
    sendError("move failed");
    return;
  }
  sendOk();
}

static void handleSettingsGet() {
  JsonDocument doc;
  buildSettingsDoc(doc);
  logSerial.print(F("{\"ok\":true,\"settings\":"));
  serializeJson(doc, logSerial);
  logSerial.print(F("}\n"));
}

static void handleSettingsSet(JsonObjectConst incoming) {
  JsonDocument merged;
  buildSettingsDoc(merged);
  for (auto kv : incoming) {
    merged[kv.key()] = kv.value();
  }
  String mergedJson;
  serializeJson(merged, mergedJson);

  JsonSettingsIO::loadSettings(SETTINGS, mergedJson.c_str());

  bool saved = false;
  {
    SpiBusMutex::Guard guard;
    saved = SETTINGS.saveToFile();
  }
  if (!saved) {
    sendError("settings save failed");
    return;
  }
  sendOk();
}

// Android expects:
// {"ok":true,"books":[{"path":"...","title":"...","author":"...","last_position":"...","last_opened":0}]}
static void handleRecent() {
  logSerial.print(F("{\"ok\":true,\"books\":["));
  bool first = true;
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (!first) logSerial.write(',');
    first = false;

    // Resolve cover BMP path (same lookup order as handleCover)
    std::string coverPath = book.coverBmpPath;
    if (coverPath.empty()) {
      core::FeatureModules::tryGetDocumentCoverPath(String(book.path.c_str()), coverPath);
    }

    JsonDocument entry;
    entry["path"] = book.path.c_str();
    entry["title"] = book.title.c_str();
    entry["author"] = book.author.c_str();
    entry["last_position"] = "";
    entry["last_opened"] = (uint32_t)0;  // not yet persisted

    BookProgressDataStore::ProgressData progress;
    if (BookProgressDataStore::loadProgress(book.path, progress)) {
      entry["last_position"] = BookProgressDataStore::formatPositionLabel(progress);
    }

    if (coverPath.empty()) {
      serializeJson(entry, logSerial);
    } else {
      // Serialize metadata to a temp string, strip trailing '}', then stream
      // cover data inline so the app receives everything in one response.
      String metaJson;
      serializeJson(entry, metaJson);
      // Remove the closing '}' so we can append the cover field
      logSerial.write(metaJson.c_str(), metaJson.length() - 1);
      logSerial.print(F(",\"cover\":\""));

      FsFile coverFile;
      bool opened = false;
      {
        SpiBusMutex::Guard guard;
        opened = Storage.openFileForRead("USB", coverPath.c_str(), coverFile);
      }
      if (opened) {
        streamFileBase64(coverFile);
        {
          SpiBusMutex::Guard guard;
          coverFile.close();
        }
      }
      logSerial.print(F("\"}"));
    }
  }
  logSerial.print(F("]}\n"));
}

// Android expects: {"ok":true,"data":"<base64-encoded BMP>"} or {"ok":false,"error":"..."}
static void handleCover(const char* path) {
  if (!PathUtils::isValidSdPath(String(path))) {
    sendError("invalid path");
    return;
  }

  // Find cached cover BMP path (check recent books first, then feature modules)
  std::string coverPath;
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (book.path == path && !book.coverBmpPath.empty()) {
      coverPath = book.coverBmpPath;
      break;
    }
  }
  if (coverPath.empty()) {
    core::FeatureModules::tryGetDocumentCoverPath(String(path), coverPath);
  }

  if (coverPath.empty()) {
    sendError("no cover available");
    return;
  }

  FsFile file;
  bool opened = false;
  {
    SpiBusMutex::Guard guard;
    opened = Storage.openFileForRead("USB", coverPath.c_str(), file);
  }

  if (!opened) {
    sendError("cover file not found");
    return;
  }

  logSerial.print(F("{\"ok\":true,\"data\":\""));
  streamFileBase64(file);
  {
    SpiBusMutex::Guard guard;
    file.close();
  }
  logSerial.print(F("\"}\n"));
}

// Returns images in /sleep/ as {"ok":true,"images":[{"path":...,"name":...},...]}
static void handleSleepList() {
  FsFile dir;
  {
    SpiBusMutex::Guard guard;
    dir = Storage.open("/sleep");
  }

  logSerial.print(F("{\"ok\":true,\"images\":["));
  bool first = true;

  if (dir && dir.isDirectory()) {
    while (true) {
      char name[256] = {0};
      bool entryIsDir = false;

      {
        SpiBusMutex::Guard guard;
        FsFile file = dir.openNextFile();
        if (!file) break;
        file.getName(name, sizeof(name));
        entryIsDir = file.isDirectory();
        file.close();
      }

      if (entryIsDir) continue;
      if (name[0] == '.') continue;

      String fname(name);
      fname.toLowerCase();
      bool supported = fname.endsWith(".bmp");
#if ENABLE_IMAGE_SLEEP
      supported = supported || fname.endsWith(".png") || fname.endsWith(".jpg") || fname.endsWith(".jpeg");
#endif
      if (!supported) continue;

      if (!first) logSerial.write(',');
      first = false;

      JsonDocument entry;
      entry["path"] = String("/sleep/") + name;
      entry["name"] = name;
      serializeJson(entry, logSerial);
    }
    {
      SpiBusMutex::Guard guard;
      dir.close();
    }
  }

  logSerial.print(F("]}\n"));
}

// Returns the currently pinned sleep cover path (empty strings when no pin is set).
static void handleSleepGetPinned() {
  const char* path = SETTINGS.sleepPinnedPath;
  const char* name = path;
  for (const char* p = path; *p; ++p) {
    if (*p == '/') name = p + 1;
  }
  JsonDocument resp;
  resp["path"] = path;
  resp["name"] = name;
  serializeJson(resp, logSerial);
  logSerial.write('\n');
}

// Pins the given sleep folder image, or clears the pin when path is empty.
static void handleSleepPin(const char* path) {
  if (!path) path = "";

  if (path[0] != '\0') {
    if (!PathUtils::isValidSdPath(String(path))) {
      sendError("invalid path");
      return;
    }
    bool exists = false;
    {
      SpiBusMutex::Guard guard;
      exists = Storage.exists(path);
    }
    if (!exists) {
      sendError("file not found");
      return;
    }
    strncpy(SETTINGS.sleepPinnedPath, path, sizeof(SETTINGS.sleepPinnedPath) - 1);
    SETTINGS.sleepPinnedPath[sizeof(SETTINGS.sleepPinnedPath) - 1] = '\0';
  } else {
    SETTINGS.sleepPinnedPath[0] = '\0';
  }

  bool saved = false;
  {
    SpiBusMutex::Guard guard;
    saved = SETTINGS.saveToFile();
  }
  if (!saved) {
    sendError("settings save failed");
    return;
  }
  sendOk();
}

// Append a todo or agenda entry to today's daily file.
// Mirrors the logic in CrossPointWebServer::handleTodoEntry().
static void handleTodoAdd(const char* text, const char* type) {
  if (!core::FeatureModules::hasCapability(core::Capability::TodoPlanner)) {
    sendError("todo_planner disabled");
    return;
  }

  // Normalize: collapse newlines to spaces, trim whitespace
  std::string normalized(text ? text : "");
  for (char& c : normalized) {
    if (c == '\r' || c == '\n') c = ' ';
  }
  const size_t s = normalized.find_first_not_of(" \t");
  const size_t e = normalized.find_last_not_of(" \t");
  if (s == std::string::npos) {
    sendError("empty text");
    return;
  }
  normalized = normalized.substr(s, e - s + 1);
  if (normalized.size() > 300) normalized.resize(300);

  const bool agendaEntry = type && strcmp(type, "agenda") == 0;
  const std::string today = DateUtils::currentDate();
  if (today.empty()) {
    sendError("date unavailable");
    return;
  }

  const std::string markdownPath = "/daily/" + today + ".md";
  const std::string textPath = "/daily/" + today + ".txt";
  const std::string dirPath = "/daily";
  std::string content;
  std::string targetPath;
  bool writeOk = false;
  {
    SpiBusMutex::Guard guard;
    const bool mdExists = Storage.exists(markdownPath.c_str());
    const bool txtExists = Storage.exists(textPath.c_str());
    targetPath = TodoPlannerStorage::dailyPath(
        today, core::FeatureModules::hasCapability(core::Capability::MarkdownSupport), mdExists, txtExists);
    if (!Storage.exists(dirPath.c_str())) Storage.mkdir(dirPath.c_str());
    if (Storage.exists(targetPath.c_str())) {
      content = Storage.readFile(targetPath.c_str()).c_str();
      if (!content.empty() && content.back() != '\n') content.push_back('\n');
    }
    content += TodoPlannerStorage::formatEntry(normalized, agendaEntry,
                                               core::FeatureModules::hasCapability(core::Capability::MarkdownSupport));
    content.push_back('\n');
    writeOk = Storage.writeFile(targetPath.c_str(), content.c_str());
  }

  if (!writeOk) {
    sendError("write failed");
    return;
  }
  sendOk();
}

// Saves credentials so they're picked up on next WiFi connection attempt.
static void handleWifiConnect(const char* ssid, const char* password) {
  if (!ssid || ssid[0] == '\0') {
    sendError("SSID required");
    return;
  }
  WIFI_STORE.addCredential(ssid, password ? password : "");
  WIFI_STORE.saveToFile();
  sendOk();
}

// ── OTA flash over USB ─────────────────────────────────────────────────────
//
// Protocol (Android → device):
//   {"cmd":"ota_begin","arg":{"size":<bytes>}}   — open next OTA partition
//   {"cmd":"ota_chunk","arg":{"data":"<base64>"}} — write decoded bytes (≤3072/call)
//   {"cmd":"ota_end"}                             — validate, set boot partition, reboot
//   {"cmd":"ota_abort"}                           — cancel in-progress OTA
//
// After ota_end the device sends {"ok":true} then restarts ~200 ms later.

static void handleOtaBegin(uint32_t /*sizeHint*/) {
  // Clean up any previous partial OTA.
  if (s_otaInProgress) {
    esp_ota_abort(s_otaHandle);
    s_otaHandle = 0;
    s_otaInProgress = false;
  }
  s_otaPartition = esp_ota_get_next_update_partition(nullptr);
  if (!s_otaPartition) {
    sendError("no OTA partition available");
    return;
  }
  // OTA_WITH_SEQUENTIAL_WRITES: erase sectors on demand — no upfront erase stall.
  const esp_err_t err = esp_ota_begin(s_otaPartition, OTA_WITH_SEQUENTIAL_WRITES, &s_otaHandle);
  if (err != ESP_OK) {
    sendError(esp_err_to_name(err));
    return;
  }
  s_otaInProgress = true;
  sendOk();
}

static void handleOtaChunk(const char* b64data) {
  if (!s_otaInProgress) {
    sendError("no OTA in progress");
    return;
  }
  const size_t b64len = strlen(b64data);
  size_t decodedLen = 0;
  const int rc =
      mbedtls_base64_decode(s_otaDecodeBuf, sizeof(s_otaDecodeBuf), &decodedLen, (const uint8_t*)b64data, b64len);
  if (rc != 0) {
    esp_ota_abort(s_otaHandle);
    s_otaHandle = 0;
    s_otaInProgress = false;
    sendError("base64 decode error");
    return;
  }
  const esp_err_t err = esp_ota_write(s_otaHandle, s_otaDecodeBuf, decodedLen);
  if (err != ESP_OK) {
    esp_ota_abort(s_otaHandle);
    s_otaHandle = 0;
    s_otaInProgress = false;
    sendError(esp_err_to_name(err));
    return;
  }
  sendOk();
}

static void handleOtaEnd() {
  if (!s_otaInProgress) {
    sendError("no OTA in progress");
    return;
  }
  esp_err_t err = esp_ota_end(s_otaHandle);
  s_otaHandle = 0;
  s_otaInProgress = false;
  if (err != ESP_OK) {
    sendError(esp_err_to_name(err));
    return;
  }
  err = esp_ota_set_boot_partition(s_otaPartition);
  if (err != ESP_OK) {
    sendError(esp_err_to_name(err));
    return;
  }
  // ACK before rebooting so Android receives the success response.
  sendOk();
  logSerial.flush();
  delay(200);
  esp_restart();
}

static void handleOtaAbort() {
  if (s_otaInProgress) {
    esp_ota_abort(s_otaHandle);
    s_otaHandle = 0;
    s_otaInProgress = false;
  }
  sendOk();  // idempotent: no-op if not in progress
}

// ── Command dispatcher ─────────────────────────────────────────────────────

static void processCommand(const char* line) {
  JsonDocument cmd;
  const auto err = deserializeJson(cmd, line);
  if (err) {
    sendError("parse error");
    return;
  }

  const char* name = cmd["cmd"] | "";

  if (strcmp(name, "status") == 0) {
    handleStatus();
  } else if (strcmp(name, "plugins") == 0) {
    handlePlugins();
  } else if (strcmp(name, "list") == 0) {
    handleList(cmd["arg"] | "/");
  } else if (strcmp(name, "download") == 0) {
    handleDownload(cmd["arg"] | "");
  } else if (strcmp(name, "upload_start") == 0) {
    const JsonObjectConst arg = cmd["arg"].as<JsonObjectConst>();
    handleUploadStart(arg["name"] | "", arg["path"] | "/", arg["size"] | (uint32_t)0);
  } else if (strcmp(name, "upload_chunk") == 0) {
    handleUploadChunk(cmd["arg"]["data"] | "");
  } else if (strcmp(name, "upload_done") == 0) {
    handleUploadDone();
  } else if (strcmp(name, "delete") == 0) {
    handleDelete(cmd["arg"].as<JsonArrayConst>());
  } else if (strcmp(name, "mkdir") == 0) {
    handleMkdir(cmd["arg"] | "");
  } else if (strcmp(name, "rename") == 0) {
    const JsonObjectConst arg = cmd["arg"].as<JsonObjectConst>();
    handleRename(arg["from"] | "", arg["to"] | "");
  } else if (strcmp(name, "move") == 0) {
    const JsonObjectConst arg = cmd["arg"].as<JsonObjectConst>();
    handleMove(arg["from"] | "", arg["to"] | "");
  } else if (strcmp(name, "settings_get") == 0) {
    handleSettingsGet();
  } else if (strcmp(name, "settings_set") == 0) {
    handleSettingsSet(cmd["arg"].as<JsonObjectConst>());
  } else if (strcmp(name, "recent") == 0) {
    handleRecent();
  } else if (strcmp(name, "cover") == 0) {
    handleCover(cmd["arg"] | "");
  } else if (strcmp(name, "sleep_list") == 0) {
    handleSleepList();
  } else if (strcmp(name, "sleep_get_pinned") == 0) {
    handleSleepGetPinned();
  } else if (strcmp(name, "sleep_pin") == 0) {
    handleSleepPin(cmd["arg"]["path"] | "");
  } else if (strcmp(name, "open_book") == 0) {
    handleOpenBook(cmd["arg"] | "");
  } else if (strcmp(name, "remote_button") == 0) {
    handleRemoteButton(cmd["arg"] | "");
  } else if (strcmp(name, "wifi_connect") == 0) {
    const JsonObjectConst arg = cmd["arg"].as<JsonObjectConst>();
    handleWifiConnect(arg["ssid"] | "", arg["password"] | "");
  } else if (strcmp(name, "wifi_status") == 0) {
    handleWifiStatus();
  } else if (strcmp(name, "todo_add") == 0) {
    const JsonObjectConst arg = cmd["arg"].as<JsonObjectConst>();
    handleTodoAdd(arg["text"] | "", arg["type"] | "todo");
  } else if (strcmp(name, "ota_begin") == 0) {
    handleOtaBegin(cmd["arg"]["size"] | (uint32_t)0);
  } else if (strcmp(name, "ota_chunk") == 0) {
    handleOtaChunk(cmd["arg"]["data"] | "");
  } else if (strcmp(name, "ota_end") == 0) {
    handleOtaEnd();
  } else if (strcmp(name, "ota_abort") == 0) {
    handleOtaAbort();
  } else {
    sendError("unknown command");
  }
}

}  // namespace

// ── Public interface ───────────────────────────────────────────────────────

void UsbSerialProtocol::loop() {
  while (logSerial.available()) {
    const int c = logSerial.read();
    if (c < 0) break;
    if (c == '\r') continue;  // tolerate CRLF line endings
    if (c == '\n') {
      s_lineBuf[s_lineLen] = '\0';
      if (s_lineLen > 0) processCommand(s_lineBuf);
      s_lineLen = 0;
      return;  // process one command per loop() call
    }
    if (s_lineLen < static_cast<int>(sizeof(s_lineBuf)) - 1) {
      s_lineBuf[s_lineLen++] = static_cast<char>(c);
    } else {
      s_lineLen = 0;  // line too long — reset and wait for next newline
    }
  }
}

void UsbSerialProtocol::reset() {
  s_lineLen = 0;
  if (s_uploadInProgress) {
    SpiBusMutex::Guard guard;
    s_uploadFile.close();
    s_uploadInProgress = false;
  }
  if (s_otaInProgress) {
    esp_ota_abort(s_otaHandle);
    s_otaHandle = 0;
    s_otaInProgress = false;
  }
}

#endif  // ENABLE_USB_MASS_STORAGE
