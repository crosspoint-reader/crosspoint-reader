#include "CrossPointWebServer.h"

#include <ArduinoJson.h>
#include <FeatureFlags.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "SettingsList.h"
#include "SpiBusMutex.h"
#include "WebDAVHandler.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/todo/TodoPlannerStorage.h"
#include "core/features/FeatureModules.h"
#include "html/FilesPageHtml.generated.h"
#include "html/HomePageHtml.generated.h"
#include "html/SettingsPageHtml.generated.h"
#include "util/AnkiStore.h"
#include "util/BookProgressDataStore.h"
#include "util/DateUtils.h"
#include "util/InputValidation.h"
#include "util/PathUtils.h"
#include "util/PokemonBookDataStore.h"

namespace {
// Folders/files to hide from the web interface file browser
// Note: Items starting with "." are automatically hidden
const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
constexpr size_t HIDDEN_ITEMS_COUNT = sizeof(HIDDEN_ITEMS) / sizeof(HIDDEN_ITEMS[0]);
constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
constexpr uint8_t CROSSPOINT_PROTOCOL_VERSION = 1;
constexpr uint16_t LOCAL_UDP_PORT = 8134;
constexpr size_t TODO_ENTRY_MAX_TEXT_LENGTH = 300;
constexpr size_t WS_CONTROL_MESSAGE_MAX_BYTES = 1024;
constexpr size_t WS_UPLOAD_MAX_BYTES = 512UL * 1024UL * 1024UL;

// Static pointer for WebSocket callback (WebSocketsServer requires C-style callback)
CrossPointWebServer* wsInstance = nullptr;

// Helper to invalidate sleep image cache when /sleep/ or sleep images are modified
void invalidateSleepCacheIfNeeded(const String& filePath) {
  String lowerPath = filePath;
  lowerPath.toLowerCase();
  if (lowerPath.equals("/sleep.bmp") || lowerPath.equals("/sleep.png") || lowerPath.equals("/sleep.jpg") ||
      lowerPath.equals("/sleep.jpeg") || lowerPath.startsWith("/sleep/") || lowerPath.equals("/sleep")) {
    invalidateSleepImageCache();
  }
}

void invalidateFeatureCachesIfNeeded(const String& filePath) {
  core::FeatureModules::onWebFileChanged(filePath);
  invalidateSleepCacheIfNeeded(filePath);
}

std::string normalizeTodoEntryText(const std::string& input) {
  std::string normalized;
  normalized.reserve(input.size());

  for (const char c : input) {
    if (c == '\r' || c == '\n') {
      normalized.push_back(' ');
    } else {
      normalized.push_back(c);
    }
  }

  size_t start = 0;
  while (start < normalized.size() && std::isspace(static_cast<unsigned char>(normalized[start]))) {
    start++;
  }
  size_t end = normalized.size();
  while (end > start && std::isspace(static_cast<unsigned char>(normalized[end - 1]))) {
    end--;
  }

  std::string trimmed = normalized.substr(start, end - start);
  if (trimmed.size() > TODO_ENTRY_MAX_TEXT_LENGTH) {
    trimmed.resize(TODO_ENTRY_MAX_TEXT_LENGTH);
  }
  return trimmed;
}

void appendTodoItemFromLine(JsonArray& array, std::string line) {
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }

  if (line.empty()) {
    return;
  }

  JsonObject item = array.add<JsonObject>();
  if (line.rfind("- [ ] ", 0) == 0) {
    item["text"] = line.substr(6);
    item["type"] = "todo";
    item["checked"] = false;
    item["isHeader"] = false;
  } else if (line.rfind("- [x] ", 0) == 0 || line.rfind("- [X] ", 0) == 0) {
    item["text"] = line.substr(6);
    item["type"] = "todo";
    item["checked"] = true;
    item["isHeader"] = false;
  } else if (line.rfind("> ", 0) == 0) {
    // Markdown blockquote — agenda entry written when markdown is enabled.
    item["text"] = line.substr(2);
    item["type"] = "agenda";
    item["checked"] = false;
    item["isHeader"] = true;
  } else {
    item["text"] = line;
    item["type"] = "text";
    item["checked"] = false;
    item["isHeader"] = true;
  }
}

void appendProgressJson(JsonObject target, const BookProgressDataStore::ProgressData& progress) {
  target["format"] = BookProgressDataStore::kindName(progress.kind);
  target["percent"] = std::round(progress.percent * 100.0f) / 100.0f;
  target["page"] = progress.page;
  target["pageCount"] = progress.pageCount;
  target["position"] = BookProgressDataStore::formatPositionLabel(progress);
  if (progress.spineIndex >= 0) {
    target["spineIndex"] = progress.spineIndex;
  }
}

}  // namespace

// File listing page template - now using generated headers:
// - HomePageHtml (from html/HomePage.html)
// - FilesPageHeaderHtml (from html/FilesPageHeader.html)
// - FilesPageFooterHtml (from html/FilesPageFooter.html)
CrossPointWebServer::CrossPointWebServer() {}

CrossPointWebServer::~CrossPointWebServer() { stop(); }

void CrossPointWebServer::begin() {
  if (running) {
    LOG_DBG("WEB", "Web server already running");
    return;
  }

  // Check if we have a valid network connection (either STA connected or AP mode)
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  const bool isInApMode = (wifiMode & WIFI_MODE_AP) && (WiFi.softAPgetStationNum() >= 0);  // AP is running

  if (!isStaConnected && !isInApMode) {
    LOG_DBG("WEB", "Cannot start webserver - no valid network (mode=%d, status=%d)", wifiMode, WiFi.status());
    return;
  }

  // Store AP mode flag for later use (e.g., in handleStatus)
  apMode = isInApMode;

  LOG_DBG("WEB", "[MEM] Free heap before begin: %d bytes", ESP.getFreeHeap());
  LOG_DBG("WEB", "Network mode: %s", apMode ? "AP" : "STA");

  LOG_DBG("WEB", "Creating web server on port %d...", port);
  server.reset(new WebServer(port));

  // Disable WiFi sleep to improve responsiveness and prevent 'unreachable' errors.
  // This is critical for reliable web server operation on ESP32.
  WiFi.setSleep(false);

  // Note: WebServer class doesn't have setNoDelay() in the standard ESP32 library.
  // We rely on disabling WiFi sleep for responsiveness.

  LOG_DBG("WEB", "[MEM] Free heap after WebServer allocation: %d bytes", ESP.getFreeHeap());

  if (!server) {
    LOG_ERR("WEB", "Failed to create WebServer!");
    return;
  }

  // Setup routes
  LOG_DBG("WEB", "Setting up routes...");
  server->on("/", HTTP_GET, [this] { handleRoot(); });
  server->on("/files", HTTP_GET, [this] { handleFileList(); });

  server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  server->on("/api/plugins", HTTP_GET, [this] { handlePlugins(); });
  // Backward-compatible alias while tooling migrates terminology.
  server->on("/api/features", HTTP_GET, [this] { handlePlugins(); });
  server->on("/api/todo/entry", HTTP_POST, [this] { handleTodoEntry(); });
  server->on("/api/todo/today", HTTP_GET, [this] { handleTodoTodayGet(); });
  server->on("/api/todo/today", HTTP_POST, [this] { handleTodoTodaySave(); });
  server->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
  server->on("/download", HTTP_GET, [this] { handleDownload(); });

  // Upload endpoint with special handling for multipart form data
  server->on("/upload", HTTP_POST, [this] { handleUploadPost(); }, [this] { handleUpload(); });

  // Create folder endpoint
  server->on("/mkdir", HTTP_POST, [this] { handleCreateFolder(); });

  // Rename file endpoint
  server->on("/rename", HTTP_POST, [this] { handleRename(); });

  // Move file endpoint
  server->on("/move", HTTP_POST, [this] { handleMove(); });

  // Delete file/folder endpoint
  server->on("/delete", HTTP_POST, [this] { handleDelete(); });

  // Settings endpoints
  server->on("/settings", HTTP_GET, [this] { handleSettingsPage(); });
  if (core::FeatureModules::shouldRegisterWebRoute(core::WebOptionalRoute::PokedexPluginPage)) {
    server->on("/plugins/pokedex", HTTP_GET, [this] { handlePokedexPluginPage(); });
  }
  if (core::FeatureModules::shouldRegisterWebRoute(core::WebOptionalRoute::PokemonPartyApi)) {
    server->on("/api/book-pokemon", HTTP_GET, [this] { handleGetBookPokemon(); });
    server->on("/api/book-pokemon", HTTP_PUT, [this] { handlePutBookPokemon(); });
    server->on("/api/book-pokemon", HTTP_DELETE, [this] { handleDeleteBookPokemon(); });
  }
  if (core::FeatureModules::shouldRegisterWebRoute(core::WebOptionalRoute::WallpaperPluginPage)) {
    server->on("/plugins/wallpaper", HTTP_GET, [this] { handleWallpaperPluginPage(); });
  }
  if (core::FeatureModules::shouldRegisterWebRoute(core::WebOptionalRoute::AnkiPluginPage)) {
    server->on("/plugins/anki", HTTP_GET, [this] { handleAnkiPluginPage(); });
    server->on("/api/anki/cards", HTTP_GET, [this] { handleAnkiGetCards(); });
    server->on("/api/anki/clear", HTTP_POST, [this] { handleAnkiClearCards(); });
  }
  server->on("/api/settings", HTTP_GET, [this] { handleGetSettings(); });
  server->on("/api/settings", HTTP_POST, [this] { handlePostSettings(); });
  if (core::FeatureModules::shouldRegisterWebRoute(core::WebOptionalRoute::UserFontsApi)) {
    server->on("/api/user-fonts/rescan", HTTP_POST, [this] { handleRescanUserFonts(); });
    server->on("/api/user-fonts/upload", HTTP_POST, [this] { handleFontUploadPost(); }, [this] { handleFontUpload(); });
  }

  // WiFi endpoints
  if (core::FeatureModules::shouldRegisterWebRoute(core::WebOptionalRoute::WebWifiSetupApi)) {
    server->on("/api/wifi/scan", HTTP_GET, [this] { handleWifiScan(); });
    server->on("/api/wifi/connect", HTTP_POST, [this] { handleWifiConnect(); });
    server->on("/api/wifi/forget", HTTP_POST, [this] { handleWifiForget(); });
    server->on("/api/wifi/status", HTTP_GET, [this] { handleWifiStatus(); });
  }

  // API endpoints for web UI (recent books, cover images, sleep cover picker)
  server->on("/api/book-progress", HTTP_GET, [this] { handleGetBookProgress(); });
  server->on("/api/recent", HTTP_GET, [this] { handleRecentBooks(); });
  server->on("/api/cover", HTTP_GET, [this] { handleCover(); });
  server->on("/api/sleep-images", HTTP_GET, [this] { handleSleepImages(); });
  server->on("/api/sleep-cover", HTTP_GET, [this] { handleSleepCoverGet(); });
  server->on("/api/sleep-cover/pin", HTTP_POST, [this] { handleSleepCoverPin(); });
  server->on("/api/open-book", HTTP_POST, [this] { handleOpenBook(); });
  server->on("/api/settings/raw", HTTP_GET, [this] { handleGetSettingsRaw(); });
  server->on("/api/remote/button", HTTP_POST, [this] { handleRemoteButton(); });
  if (core::FeatureModules::shouldRegisterWebRoute(core::WebOptionalRoute::OtaApi)) {
    server->on("/api/ota/check", HTTP_POST, [this] { handleOtaCheckPost(); });
    server->on("/api/ota/check", HTTP_GET, [this] { handleOtaCheckGet(); });
  }

  server->onNotFound([this] { handleNotFound(); });
  LOG_DBG("WEB", "[MEM] Free heap after route setup: %d bytes", ESP.getFreeHeap());

#if CROSSPOINT_HAS_NETWORKUDP
  // Collect WebDAV headers and register handler
  const char* davHeaders[] = {"Depth", "Destination", "Overwrite", "If", "Lock-Token", "Timeout"};
  server->collectHeaders(davHeaders, 6);
  server->addHandler(new WebDAVHandler());  // Note: WebDAVHandler will be deleted by WebServer when server is stopped
  LOG_DBG("WEB", "WebDAV handler initialized");
#endif

  server->begin();

  // Start WebSocket server for fast binary uploads
  LOG_DBG("WEB", "Starting WebSocket server on port %d...", wsPort);
  wsServer.reset(new WebSocketsServer(wsPort));
  wsInstance = const_cast<CrossPointWebServer*>(this);
  wsServer->begin();
  wsServer->onEvent(wsEventCallback);
  LOG_DBG("WEB", "WebSocket server started");

  udpActive = udp.begin(LOCAL_UDP_PORT);
  LOG_DBG("WEB", "Discovery UDP %s on port %d", udpActive ? "enabled" : "failed", LOCAL_UDP_PORT);

  running = true;

  LOG_DBG("WEB", "Web server started on port %d", port);
  // Show the correct IP based on network mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  LOG_DBG("WEB", "Access at http://%s/", ipAddr.c_str());
  LOG_DBG("WEB", "WebSocket at ws://%s:%d/", ipAddr.c_str(), wsPort);
  LOG_DBG("WEB", "[MEM] Free heap after server.begin(): %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::stop() {
  if (!running || !server) {
    LOG_DBG("WEB", "stop() called but already stopped (running=%d, server=%p)", running, server.get());
    return;
  }

  LOG_DBG("WEB", "STOP INITIATED - setting running=false first");
  running = false;  // Set this FIRST to prevent handleClient from using server

  LOG_DBG("WEB", "[MEM] Free heap before stop: %d bytes", ESP.getFreeHeap());

  // Close any in-progress WebSocket upload
  if (wsUploadInProgress && wsUploadFile) {
    wsUploadFile.close();
    wsUploadInProgress = false;
  }
  wsUploadOwnerValid = false;
  wsUploadOwnerClient = 0;

  // Close any in-progress HTTP upload
  if (uploadFile) {
    uploadFile.close();
  }
  uploadBufferPos = 0;
  uploadSuccess = false;
  uploadError = "";
  uploadLastLoggedSize = 0;

  // Stop WebSocket server
  if (wsServer) {
    LOG_DBG("WEB", "Stopping WebSocket server...");
    wsServer->close();
    wsServer.reset();
    wsInstance = nullptr;
    LOG_DBG("WEB", "WebSocket server stopped");
  }

  if (udpActive) {
    udp.stop();
    udpActive = false;
  }

  // Brief delay to allow any in-flight handleClient() calls to complete
  delay(20);

  server->stop();
  LOG_DBG("WEB", "[MEM] Free heap after server->stop(): %d bytes", ESP.getFreeHeap());

  // Brief delay before deletion
  delay(10);

  server.reset();
  LOG_DBG("WEB", "Web server stopped and deleted");
  LOG_DBG("WEB", "[MEM] Free heap after delete server: %d bytes", ESP.getFreeHeap());
  LOG_DBG("WEB", "[MEM] Free heap final: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::handleClient() {
  static unsigned long lastDebugPrint = 0;

  // Check running flag FIRST before accessing server
  if (!running) {
    return;
  }

  // Double-check server pointer is valid
  if (!server) {
    LOG_DBG("WEB", "WARNING: handleClient called with null server!");
    return;
  }

  // Print debug every 10 seconds to confirm handleClient is being called
  if (millis() - lastDebugPrint > 10000) {
    LOG_DBG("WEB", "handleClient active, server running on port %d", port);
    lastDebugPrint = millis();
  }

  server->handleClient();

  // Handle WebSocket events
  if (wsServer) {
    wsServer->loop();
  }

  // Respond to discovery broadcasts
  if (udpActive) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      char buffer[16];
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        if (strcmp(buffer, "hello") == 0) {
          String hostname = WiFi.getHostname();
          if (hostname.isEmpty()) {
            hostname = "crosspoint";
          }
          String message = "crosspoint (on " + hostname + ");" + String(wsPort);
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write(reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
          udp.endPacket();
        }
      }
    }
  }
}

CrossPointWebServer::WsUploadStatus CrossPointWebServer::getWsUploadStatus() const {
  WsUploadStatus status;
  status.inProgress = wsUploadInProgress;
  status.received = wsUploadReceived;
  status.total = wsUploadSize;
  status.filename = wsUploadFileName.c_str();
  status.lastCompleteName = wsLastCompleteName.c_str();
  status.lastCompleteSize = wsLastCompleteSize;
  status.lastCompleteAt = wsLastCompleteAt;
  return status;
}

static_assert(HomePageHtmlCompressedSize == sizeof(HomePageHtml), "Home page compressed size mismatch");
static_assert(FilesPageHtmlCompressedSize == sizeof(FilesPageHtml), "Files page compressed size mismatch");
static_assert(SettingsPageHtmlCompressedSize == sizeof(SettingsPageHtml), "Settings page compressed size mismatch");

static bool isGzipPayload(const char* data, size_t len) {
  return len >= 2 && static_cast<unsigned char>(data[0]) == 0x1f && static_cast<unsigned char>(data[1]) == 0x8b;
}

static bool parseStrictSize(const String& token, size_t& outValue) {
  return InputValidation::parseStrictPositiveSize(token.c_str(), token.length(), WS_UPLOAD_MAX_BYTES, outValue);
}

static void sendPrecompressedHtml(WebServer* server, const char* data, size_t compressedLen) {
  if (!isGzipPayload(data, compressedLen)) {
    LOG_ERR("WEB", "Attempted to serve non-gzip HTML payload");
    server->send(500, "text/plain", "Invalid precompressed HTML payload");
    return;
  }
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "text/html; charset=utf-8", data, compressedLen);
}

void CrossPointWebServer::handleRoot() const {
  sendPrecompressedHtml(server.get(), HomePageHtml, HomePageHtmlCompressedSize);
  LOG_DBG("WEB", "Served root page");
}

void CrossPointWebServer::handleNotFound() const {
  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

void CrossPointWebServer::handleStatus() const {
  requestCount++;
  // Get correct IP based on AP vs STA mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  const bool staConnected = WiFi.status() == WL_CONNECTED;
  const String wifiStatus = apMode ? "AP Mode" : (staConnected ? "Connected" : "Disconnected");

  JsonDocument doc;
  doc["version"] = CROSSPOINT_VERSION;
  doc["protocolVersion"] = CROSSPOINT_PROTOCOL_VERSION;
  doc["wifiStatus"] = wifiStatus;
  doc["ip"] = ipAddr;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["rssi"] = apMode ? 0 : WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;
  doc["openBook"] = APP_STATE.openEpubPath.c_str();
  doc["otaSelectedBundle"] = SETTINGS.selectedOtaBundle;
  doc["otaInstalledBundle"] = SETTINGS.installedOtaBundle;
  doc["otaInstalledFeatures"] = SETTINGS.installedOtaFeatureFlags[0] != '\0' ? SETTINGS.installedOtaFeatureFlags
                                                                             : core::FeatureModules::getBuildString();

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handlePlugins() const {
  server->send(200, "application/json", core::FeatureModules::getFeatureMapJson());
}

void CrossPointWebServer::handleTodoEntry() {
  if (!core::FeatureModules::hasCapability(core::Capability::TodoPlanner)) {
    server->send(404, "text/plain", "TODO planner disabled");
    return;
  }

  if (!server->hasArg("text")) {
    server->send(400, "text/plain", "Missing text");
    return;
  }

  String text = server->arg("text");
  text.replace("\r\n", " ");
  text.replace("\r", " ");
  text.replace("\n", " ");
  text.trim();
  if (text.isEmpty() || text.length() > TODO_ENTRY_MAX_TEXT_LENGTH) {
    server->send(400, "text/plain", "Invalid text");
    return;
  }

  const bool agendaEntry = server->arg("type").equalsIgnoreCase("agenda");
  const std::string today = DateUtils::currentDate();
  if (today.empty()) {
    server->send(503, "text/plain", "Date unavailable");
    return;
  }

  const std::string markdownPath = "/daily/" + today + ".md";
  const std::string textPath = "/daily/" + today + ".txt";
  const std::string dirPath = "/daily";

  // All SD operations serialised under one mutex guard to prevent data races
  // with concurrent tasks (e.g. TodoActivity) accessing the SPI bus.
  std::string content;
  std::string targetPath;
  bool writeOk = false;
  {
    SpiBusMutex::Guard guard;
    const bool markdownExists = Storage.exists(markdownPath.c_str());
    const bool textExists = Storage.exists(textPath.c_str());
    targetPath = TodoPlannerStorage::dailyPath(
        today, core::FeatureModules::hasCapability(core::Capability::MarkdownSupport), markdownExists, textExists);
    if (!Storage.exists(dirPath.c_str())) {
      Storage.mkdir(dirPath.c_str());
    }
    if (Storage.exists(targetPath.c_str())) {
      content = Storage.readFile(targetPath.c_str()).c_str();
      if (!content.empty() && content.back() != '\n') {
        content.push_back('\n');
      }
    }
    content += TodoPlannerStorage::formatEntry(text.c_str(), agendaEntry,
                                               core::FeatureModules::hasCapability(core::Capability::MarkdownSupport));
    content.push_back('\n');
    writeOk = Storage.writeFile(targetPath.c_str(), content.c_str());
  }

  if (!writeOk) {
    server->send(500, "text/plain", "Failed to write TODO entry");
    return;
  }

  server->send(200, "application/json", "{\"ok\":true}");
}

void CrossPointWebServer::handleTodoTodayGet() const {
  if (!core::FeatureModules::hasCapability(core::Capability::TodoPlanner)) {
    server->send(404, "text/plain", "TODO planner disabled");
    return;
  }

  const std::string today = DateUtils::currentDate();
  if (today.empty()) {
    server->send(503, "text/plain", "Date unavailable");
    return;
  }

  const std::string markdownPath = "/daily/" + today + ".md";
  const std::string textPath = "/daily/" + today + ".txt";
  std::string targetPath;
  std::string content;
  {
    SpiBusMutex::Guard guard;
    const bool markdownExists = Storage.exists(markdownPath.c_str());
    const bool textExists = Storage.exists(textPath.c_str());
    targetPath = TodoPlannerStorage::dailyPath(
        today, core::FeatureModules::hasCapability(core::Capability::MarkdownSupport), markdownExists, textExists);
    if (Storage.exists(targetPath.c_str())) {
      content = Storage.readFile(targetPath.c_str()).c_str();
    }
  }

  JsonDocument response;
  response["ok"] = true;
  response["date"] = today.c_str();
  response["path"] = targetPath.c_str();
  JsonArray items = response["items"].to<JsonArray>();

  std::string line;
  line.reserve(128);
  for (const char c : content) {
    if (c == '\n') {
      appendTodoItemFromLine(items, line);
      line.clear();
    } else {
      line.push_back(c);
    }
  }
  if (!line.empty()) {
    appendTodoItemFromLine(items, line);
  }

  String json;
  serializeJson(response, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleTodoTodaySave() const {
  if (!core::FeatureModules::hasCapability(core::Capability::TodoPlanner)) {
    server->send(404, "text/plain", "TODO planner disabled");
    return;
  }

  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing body");
    return;
  }

  JsonDocument request;
  if (deserializeJson(request, server->arg("plain"))) {
    server->send(400, "text/plain", "Invalid JSON body");
    return;
  }

  if (!request["items"].is<JsonArray>()) {
    server->send(400, "text/plain", "Missing items array");
    return;
  }

  const std::string today = DateUtils::currentDate();
  if (today.empty()) {
    server->send(503, "text/plain", "Date unavailable");
    return;
  }

  const std::string markdownPath = "/daily/" + today + ".md";
  const std::string textPath = "/daily/" + today + ".txt";
  const std::string dirPath = "/daily";
  std::string targetPath;
  std::string content;

  JsonArray items = request["items"].as<JsonArray>();
  for (JsonVariant itemVar : items) {
    if (!itemVar.is<JsonObject>()) {
      continue;
    }

    JsonObject item = itemVar.as<JsonObject>();
    const std::string text = normalizeTodoEntryText(item["text"].as<std::string>());
    if (text.empty()) {
      continue;
    }

    const bool isHeader = item["isHeader"].is<bool>() ? item["isHeader"].as<bool>() : item["is_header"].as<bool>();
    const bool checked = item["checked"].as<bool>();
    const char* itemType = item["type"] | "";
    const bool isAgenda = isHeader && strcmp(itemType, "agenda") == 0;
    const bool markdownEnabled = core::FeatureModules::hasCapability(core::Capability::MarkdownSupport);
    if (isHeader) {
      if (isAgenda && markdownEnabled) content += "> ";
      content += text;
    } else {
      content += "- [";
      content += checked ? "x" : " ";
      content += "] ";
      content += text;
    }
    content.push_back('\n');
  }

  bool writeOk = false;
  {
    SpiBusMutex::Guard guard;
    const bool markdownExists = Storage.exists(markdownPath.c_str());
    const bool textExists = Storage.exists(textPath.c_str());
    targetPath = TodoPlannerStorage::dailyPath(
        today, core::FeatureModules::hasCapability(core::Capability::MarkdownSupport), markdownExists, textExists);
    if (!Storage.exists(dirPath.c_str())) {
      Storage.mkdir(dirPath.c_str());
    }
    writeOk = Storage.writeFile(targetPath.c_str(), content.c_str());
  }

  if (!writeOk) {
    server->send(500, "text/plain", "Failed to write TODO file");
    return;
  }

  invalidateFeatureCachesIfNeeded(String(targetPath.c_str()));
  JsonDocument response;
  response["ok"] = true;
  response["date"] = today.c_str();
  response["path"] = targetPath.c_str();
  String json;
  serializeJson(response, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::scanFiles(const char* path, const std::function<void(FileInfo)>& callback) const {
  FsFile root;
  {
    SpiBusMutex::Guard guard;
    root = Storage.open(path);
  }

  if (!root) {
    LOG_DBG("WEB", "Failed to open directory: %s", path);
    return;
  }

  if (!root.isDirectory()) {
    LOG_DBG("WEB", "Not a directory: %s", path);
    root.close();
    return;
  }

  LOG_DBG("WEB", "Scanning files in: %s", path);

  while (true) {
    FileInfo info;
    bool shouldHide = false;

    // Scope SD card operations with mutex
    {
      SpiBusMutex::Guard guard;
      FsFile file = root.openNextFile();
      if (!file) {
        break;
      }

      char name[500];
      file.getName(name, sizeof(name));
      auto fileName = String(name);

      // Skip hidden items (starting with ".")
      shouldHide = fileName.startsWith(".");

      // Check against explicitly hidden items list
      if (!shouldHide) {
        for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
          if (fileName.equals(HIDDEN_ITEMS[i])) {
            shouldHide = true;
            break;
          }
        }
      }

      if (!shouldHide) {
        info.name = fileName;
        info.isDirectory = file.isDirectory();

        if (info.isDirectory) {
          info.size = 0;
          info.isEpub = false;
        } else {
          info.size = file.size();
          info.isEpub = isEpubFile(info.name);
        }
      }
      file.close();
    }

    // Callback performs network operations - run without mutex
    if (!shouldHide) {
      callback(info);
    }

    yield();               // Yield to allow WiFi and other tasks to process during long scans
    esp_task_wdt_reset();  // Reset watchdog to prevent timeout on large directories
  }

  {
    SpiBusMutex::Guard guard;
    root.close();
  }
}

bool CrossPointWebServer::isEpubFile(const String& filename) const {
  String lower = filename;
  lower.toLowerCase();
  return lower.endsWith(".epub");
}

bool CrossPointWebServer::isProtectedComponent(const String& component) const {
  if (component.isEmpty()) {
    return false;
  }
  if (component.startsWith(".")) {
    return true;
  }
  for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
    if (component.equalsIgnoreCase(HIDDEN_ITEMS[i])) {
      return true;
    }
  }
  return false;
}

bool CrossPointWebServer::pathContainsProtectedItem(const String& path) const {
  String normalized = PathUtils::normalizePath(path);
  if (normalized == "/") {
    return false;
  }

  int start = (normalized.startsWith("/")) ? 1 : 0;
  while (start < static_cast<int>(normalized.length())) {
    int slash = normalized.indexOf('/', start);
    if (slash < 0) {
      slash = normalized.length();
    }

    const String component = normalized.substring(start, slash);
    if (isProtectedComponent(component)) {
      return true;
    }
    start = slash + 1;
  }

  return false;
}

void CrossPointWebServer::handleFileList() const {
  sendPrecompressedHtml(server.get(), FilesPageHtml, FilesPageHtmlCompressedSize);
}

void CrossPointWebServer::handleFileListData() const {
  // Get current path from query string (default to root)
  String currentPath = "/";
  if (server->hasArg("path")) {
    const String rawArg = server->arg("path");
    currentPath = PathUtils::urlDecode(rawArg);
    LOG_DBG("WEB", "Files API - raw arg: '%s' (%d bytes), decoded: '%s' (%d bytes)", rawArg.c_str(),
            (int)rawArg.length(), currentPath.c_str(), (int)currentPath.length());

    // Validate path against traversal attacks
    if (!PathUtils::isValidSdPath(currentPath)) {
      LOG_WRN("WEB", "Path validation FAILED. raw='%s' (%d bytes) decoded='%s' (%d bytes)", rawArg.c_str(),
              (int)rawArg.length(), currentPath.c_str(), (int)currentPath.length());
      server->send(400, "text/plain", "Invalid path");
      return;
    }
    LOG_DBG("WEB", "Path validation OK");

    currentPath = PathUtils::normalizePath(currentPath);
    if (pathContainsProtectedItem(currentPath)) {
      server->send(403, "text/plain", "Cannot access protected items");
      return;
    }
  }

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");
  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  scanFiles(currentPath.c_str(), [this, &output, &doc, &seenFirst](const FileInfo& info) {
    doc.clear();
    doc["name"] = info.name;
    doc["size"] = info.size;
    doc["isDirectory"] = info.isDirectory;
    doc["isEpub"] = info.isEpub;

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) {
      // JSON output truncated; skip this entry to avoid sending malformed JSON
      LOG_DBG("WEB", "Skipping file entry with oversized JSON for name: %s", info.name.c_str());
      return;
    }

    if (seenFirst) {
      server->sendContent(",");
    } else {
      seenFirst = true;
    }
    server->sendContent(output);
  });
  server->sendContent("]");
  // End of streamed response, empty chunk to signal client
  server->sendContent("");
  LOG_DBG("WEB", "Served file listing page for path: %s", currentPath.c_str());
}

void CrossPointWebServer::handleDownload() const {
  requestCount++;
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  const String rawArg = server->arg("path");
  String itemPath = PathUtils::urlDecode(rawArg);
  if (!PathUtils::isValidSdPath(itemPath)) {
    LOG_WRN("WEB", "Download rejected - invalid path. raw='%s' decoded='%s'", rawArg.c_str(), itemPath.c_str());
    server->send(400, "text/plain", "Invalid path");
    return;
  }

  itemPath = PathUtils::normalizePath(itemPath);
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (pathContainsProtectedItem(itemPath)) {
    server->send(403, "text/plain", "Cannot access protected items");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (itemName.startsWith(".")) {
    server->send(403, "text/plain", "Cannot access system files");
    return;
  }
  for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
    if (itemName.equals(HIDDEN_ITEMS[i])) {
      server->send(403, "text/plain", "Cannot access protected items");
      return;
    }
  }

  bool exists = false;
  {
    SpiBusMutex::Guard guard;
    exists = Storage.exists(itemPath.c_str());
  }
  if (!exists) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file;
  {
    SpiBusMutex::Guard guard;
    file = Storage.open(itemPath.c_str());
  }
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  bool isDirectory = false;
  {
    SpiBusMutex::Guard guard;
    isDirectory = file.isDirectory();
  }
  if (isDirectory) {
    {
      SpiBusMutex::Guard guard;
      file.close();
    }
    server->send(400, "text/plain", "Path is a directory");
    return;
  }

  String contentType = "application/octet-stream";
  if (isEpubFile(itemPath)) {
    contentType = "application/epub+zip";
  }

  size_t fileSize = 0;
  {
    SpiBusMutex::Guard guard;
    fileSize = file.size();
  }

  char nameBuf[128] = {0};
  String filename = "download";
  {
    SpiBusMutex::Guard guard;
    if (file.getName(nameBuf, sizeof(nameBuf))) {
      filename = nameBuf;
    }
  }

  server->setContentLength(fileSize);
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server->send(200, contentType.c_str(), "");

  WiFiClient client = server->client();
  uint8_t buffer[1024];
  while (true) {
    size_t bytesRead = 0;
    {
      SpiBusMutex::Guard guard;
      bytesRead = file.read(buffer, sizeof(buffer));
    }
    if (bytesRead == 0) {
      break;
    }
    const size_t bytesWritten = client.write(buffer, bytesRead);
    if (bytesWritten != bytesRead) {
      LOG_WRN("WEB", "Download truncated for %s (wanted %u, wrote %u)", itemPath.c_str(),
              static_cast<unsigned int>(bytesRead), static_cast<unsigned int>(bytesWritten));
      break;
    }
    yield();
    esp_task_wdt_reset();
  }
  {
    SpiBusMutex::Guard guard;
    file.close();
  }
}

// Upload write buffer - batches small writes into larger SD card operations
// 4KB is a good balance: large enough to reduce syscall overhead, small enough
// to keep individual write times short and avoid watchdog issues
bool CrossPointWebServer::flushUploadBuffer() {
  if (uploadBufferPos > 0 && uploadFile) {
    SpiBusMutex::Guard guard;
    esp_task_wdt_reset();  // Reset watchdog before potentially slow SD write
    const unsigned long writeStart = millis();
    const size_t written = uploadFile.write(uploadBuffer, uploadBufferPos);
    totalWriteTime += millis() - writeStart;
    writeCount++;
    esp_task_wdt_reset();  // Reset watchdog after SD write

    if (written != uploadBufferPos) {
      LOG_DBG("WEB", "[UPLOAD] Buffer flush failed: expected %d, wrote %d", uploadBufferPos, written);
      uploadBufferPos = 0;
      return false;
    }
    uploadBufferPos = 0;
  }
  return true;
}

void CrossPointWebServer::handleUpload() {
  // Reset watchdog at start of every upload callback - HTTP parsing can be slow
  esp_task_wdt_reset();

  // Safety check: ensure server is still valid
  if (!running || !server) {
    LOG_DBG("WEB", "[UPLOAD] ERROR: handleUpload called but server not running!");
    return;
  }

  const HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    // Reset watchdog - this is the critical 1% crash point
    esp_task_wdt_reset();

    uploadFileName = upload.filename;
    uploadSize = 0;
    uploadSuccess = false;
    uploadError = "";
    uploadStartTime = millis();
    uploadLastLoggedSize = 0;
    uploadBufferPos = 0;
    totalWriteTime = 0;
    writeCount = 0;

    // Validate filename to prevent path traversal
    if (!PathUtils::isValidFilename(uploadFileName)) {
      uploadError = "Invalid filename";
      LOG_WRN("WEB", "[UPLOAD] Invalid filename rejected: %s", uploadFileName.c_str());
      return;
    }
    if (isProtectedComponent(uploadFileName)) {
      uploadError = "Cannot upload protected files";
      LOG_WRN("WEB", "[UPLOAD] Protected filename rejected: %s", uploadFileName.c_str());
      return;
    }

    // Get upload path from query parameter (defaults to root if not specified)
    // Note: We use query parameter instead of form data because multipart form
    // fields aren't available until after file upload completes
    if (server->hasArg("path")) {
      uploadPath = PathUtils::urlDecode(server->arg("path"));

      // Validate path against traversal attacks
      if (!PathUtils::isValidSdPath(uploadPath)) {
        uploadError = "Invalid path";
        LOG_WRN("WEB", "[UPLOAD] Path validation failed: %s", uploadPath.c_str());
        return;
      }

      uploadPath = PathUtils::normalizePath(uploadPath);
      if (pathContainsProtectedItem(uploadPath)) {
        uploadError = "Cannot upload to protected path";
        LOG_WRN("WEB", "[UPLOAD] Protected upload path rejected: %s", uploadPath.c_str());
        return;
      }
    } else {
      uploadPath = "/";
    }

    LOG_DBG("WEB", "[UPLOAD] START: %s to path: %s", uploadFileName.c_str(), uploadPath.c_str());
    LOG_DBG("WEB", "[UPLOAD] Free heap: %d bytes", ESP.getFreeHeap());

    // Create file path
    String filePath = uploadPath;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += uploadFileName;

    // Check if file already exists - SD operations can be slow
    bool hadExistingFile = false;
    esp_task_wdt_reset();
    {
      SpiBusMutex::Guard guard;
      hadExistingFile = Storage.exists(filePath.c_str());
      if (hadExistingFile) {
        Storage.remove(filePath.c_str());
      }
    }
    if (hadExistingFile) {
      LOG_DBG("WEB", "[UPLOAD] Overwriting existing file: %s", filePath.c_str());
    }

    // Open file for writing - this can be slow due to FAT cluster allocation
    esp_task_wdt_reset();
    bool opened = false;
    {
      SpiBusMutex::Guard guard;
      opened = Storage.openFileForWrite("WEB", filePath, uploadFile);
    }
    if (!opened) {
      uploadError = "Failed to create file on SD card";
      LOG_DBG("WEB", "[UPLOAD] FAILED to create file: %s", filePath.c_str());
      return;
    }
    esp_task_wdt_reset();

    LOG_DBG("WEB", "[UPLOAD] File created successfully: %s", filePath.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile && uploadError.isEmpty()) {
      // Buffer incoming data and flush when buffer is full
      // This reduces SD card write operations and improves throughput
      const uint8_t* data = upload.buf;
      size_t remaining = upload.currentSize;

      while (remaining > 0) {
        const size_t space = kUploadBufferSize - uploadBufferPos;
        const size_t toCopy = (remaining < space) ? remaining : space;

        memcpy(uploadBuffer + uploadBufferPos, data, toCopy);
        uploadBufferPos += toCopy;
        data += toCopy;
        remaining -= toCopy;

        // Flush buffer when full
        if (uploadBufferPos >= kUploadBufferSize) {
          if (!flushUploadBuffer()) {
            uploadError = "Failed to write to SD card - disk may be full";
            {
              SpiBusMutex::Guard guard;
              uploadFile.close();
            }
            return;
          }
        }
      }

      uploadSize += upload.currentSize;

      // Log progress every 100KB
      if (uploadSize - uploadLastLoggedSize >= 102400) {
        const unsigned long elapsed = millis() - uploadStartTime;
        const float kbps = (elapsed > 0) ? (uploadSize / 1024.0) / (elapsed / 1000.0) : 0;
        LOG_DBG("WEB", "[UPLOAD] %d bytes (%.1f KB), %.1f KB/s, %d writes", uploadSize, uploadSize / 1024.0, kbps,
                writeCount);
        uploadLastLoggedSize = uploadSize;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      // Flush any remaining buffered data
      if (!flushUploadBuffer()) {
        uploadError = "Failed to write final data to SD card";
      }
      {
        SpiBusMutex::Guard guard;
        uploadFile.close();
      }

      if (uploadError.isEmpty()) {
        uploadSuccess = true;
        const unsigned long elapsed = millis() - uploadStartTime;
        const float avgKbps = (elapsed > 0) ? (uploadSize / 1024.0) / (elapsed / 1000.0) : 0;
        const float writePercent = (elapsed > 0) ? (totalWriteTime * 100.0 / elapsed) : 0;
        LOG_DBG("WEB", "[UPLOAD] Complete: %s (%d bytes in %lu ms, avg %.1f KB/s)", uploadFileName.c_str(), uploadSize,
                elapsed, avgKbps);
        LOG_DBG("WEB", "[UPLOAD] Diagnostics: %d writes, total write time: %lu ms (%.1f%%)", writeCount, totalWriteTime,
                writePercent);

        // Clear epub cache to prevent stale metadata issues when overwriting files
        String filePath = uploadPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += uploadFileName;
        invalidateFeatureCachesIfNeeded(filePath);
        core::FeatureModules::onUploadCompleted(uploadPath, uploadFileName);
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    uploadBufferPos = 0;  // Discard buffered data
    if (uploadFile) {
      SpiBusMutex::Guard guard;
      uploadFile.close();
      // Try to delete the incomplete file
      String filePath = uploadPath;
      if (!filePath.endsWith("/")) filePath += "/";
      filePath += uploadFileName;
      Storage.remove(filePath.c_str());
    }
    uploadError = "Upload aborted";
    LOG_DBG("WEB", "Upload aborted");
  }
}

void CrossPointWebServer::handleUploadPost() {
  requestCount++;
  if (uploadSuccess) {
    server->send(200, "text/plain", "File uploaded successfully: " + uploadFileName);
  } else {
    const String error = uploadError.isEmpty() ? "Unknown error during upload" : uploadError;
    server->send(400, "text/plain", error);
  }
}

void CrossPointWebServer::handleCreateFolder() const {
  // Get folder name from form data
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing folder name");
    return;
  }

  const String folderName = server->arg("name");

  // Validate folder name (no path separators or traversal)
  if (!PathUtils::isValidFilename(folderName)) {
    LOG_WRN("WEB", "Invalid folder name rejected: %s", folderName.c_str());
    server->send(400, "text/plain", "Invalid folder name");
    return;
  }
  if (isProtectedComponent(folderName)) {
    server->send(403, "text/plain", "Cannot create protected folders");
    return;
  }

  // Get parent path
  String parentPath = "/";
  if (server->hasArg("path")) {
    parentPath = PathUtils::urlDecode(server->arg("path"));

    // Validate path against traversal attacks
    if (!PathUtils::isValidSdPath(parentPath)) {
      LOG_WRN("WEB", "Path validation failed for mkdir: %s", parentPath.c_str());
      server->send(400, "text/plain", "Invalid path");
      return;
    }

    parentPath = PathUtils::normalizePath(parentPath);
    if (pathContainsProtectedItem(parentPath)) {
      server->send(403, "text/plain", "Cannot access protected items");
      return;
    }
  }

  // Build full folder path
  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  LOG_DBG("WEB", "Creating folder: %s", folderPath.c_str());

  // Check if already exists
  bool folderExists = false;
  {
    SpiBusMutex::Guard guard;
    folderExists = Storage.exists(folderPath.c_str());
  }
  if (folderExists) {
    server->send(400, "text/plain", "Folder already exists");
    return;
  }

  // Create the folder while holding the SPI mutex.
  bool mkdirOk = false;
  {
    SpiBusMutex::Guard guard;
    mkdirOk = Storage.mkdir(folderPath.c_str());
  }
  if (mkdirOk) {
    LOG_DBG("WEB", "Folder created successfully: %s", folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    LOG_DBG("WEB", "Failed to create folder: %s", folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void CrossPointWebServer::handleRename() const {
  String itemPath;
  String renameTarget;
  bool fromFormContract = false;

  if (server->hasArg("path") && server->hasArg("name")) {
    itemPath = PathUtils::urlDecode(server->arg("path"));
    renameTarget = server->arg("name");
    fromFormContract = true;
  } else {
    if (!server->hasArg("plain")) {
      server->send(400, "text/plain", "Missing path or new name");
      return;
    }

    JsonDocument body;
    if (deserializeJson(body, server->arg("plain"))) {
      server->send(400, "text/plain", "Invalid JSON body");
      return;
    }

    itemPath = PathUtils::urlDecode(body["from"].as<String>());
    renameTarget = body["to"].as<String>();
  }

  renameTarget.trim();

  if (!PathUtils::isValidSdPath(itemPath)) {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  itemPath = PathUtils::normalizePath(itemPath);

  if (itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (pathContainsProtectedItem(itemPath)) {
    server->send(403, "text/plain", "Cannot rename protected item");
    return;
  }

  String parentPath = itemPath.substring(0, itemPath.lastIndexOf('/'));
  if (parentPath.isEmpty()) {
    parentPath = "/";
  }

  String newPath;
  String newName;
  if (fromFormContract || (renameTarget.indexOf('/') < 0 && renameTarget.indexOf('\\') < 0)) {
    newName = renameTarget;
    if (newName.isEmpty()) {
      server->send(400, "text/plain", "New name cannot be empty");
      return;
    }
    if (newName.indexOf('/') >= 0 || newName.indexOf('\\') >= 0) {
      server->send(400, "text/plain", "Invalid file name");
      return;
    }
    if (newName.startsWith(".")) {
      server->send(403, "text/plain", "Cannot rename to hidden name");
      return;
    }
    if (isProtectedComponent(newName)) {
      server->send(403, "text/plain", "Cannot rename to protected name");
      return;
    }

    newPath = parentPath;
    if (!newPath.endsWith("/")) {
      newPath += "/";
    }
    newPath += newName;
  } else {
    String candidatePath = PathUtils::urlDecode(renameTarget);
    if (!PathUtils::isValidSdPath(candidatePath)) {
      server->send(400, "text/plain", "Invalid path");
      return;
    }
    newPath = PathUtils::normalizePath(candidatePath);
    if (newPath.isEmpty() || newPath == "/") {
      server->send(400, "text/plain", "Invalid path");
      return;
    }
    if (pathContainsProtectedItem(newPath)) {
      server->send(403, "text/plain", "Cannot rename to protected path");
      return;
    }

    newName = newPath.substring(newPath.lastIndexOf('/') + 1);
    if (newName.isEmpty()) {
      server->send(400, "text/plain", "Invalid file name");
      return;
    }
    if (newName.startsWith(".")) {
      server->send(403, "text/plain", "Cannot rename to hidden name");
      return;
    }
    if (isProtectedComponent(newName)) {
      server->send(403, "text/plain", "Cannot rename to protected name");
      return;
    }
  }

  if (newName.isEmpty()) {
    server->send(400, "text/plain", "New name cannot be empty");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (itemName.startsWith(".")) {
    server->send(403, "text/plain", "Cannot rename protected item");
    return;
  }
  if (newName == itemName) {
    server->send(200, "text/plain", "Name unchanged");
    return;
  }

  bool itemExists = false;
  {
    SpiBusMutex::Guard guard;
    itemExists = Storage.exists(itemPath.c_str());
  }
  if (!itemExists) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file;
  {
    SpiBusMutex::Guard guard;
    file = Storage.open(itemPath.c_str());
  }
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  bool isDir = false;
  {
    SpiBusMutex::Guard guard;
    isDir = file.isDirectory();
    if (isDir) {
      file.close();
    }
  }
  if (isDir) {
    server->send(400, "text/plain", "Only files can be renamed");
    return;
  }

  bool targetExists = false;
  {
    SpiBusMutex::Guard guard;
    targetExists = Storage.exists(newPath.c_str());
  }
  if (targetExists) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  invalidateFeatureCachesIfNeeded(itemPath);
  bool success = false;
  {
    SpiBusMutex::Guard guard;
    success = file.rename(newPath.c_str());
  }
  file.close();

  if (success) {
    LOG_DBG("WEB", "Renamed file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Renamed successfully");
  } else {
    LOG_ERR("WEB", "Failed to rename file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to rename file");
  }
}

void CrossPointWebServer::handleMove() const {
  String itemPath;
  String destPath;
  if (server->hasArg("path") && server->hasArg("dest")) {
    itemPath = PathUtils::urlDecode(server->arg("path"));
    destPath = PathUtils::urlDecode(server->arg("dest"));
  } else {
    if (!server->hasArg("plain")) {
      server->send(400, "text/plain", "Missing path or destination");
      return;
    }

    JsonDocument body;
    if (deserializeJson(body, server->arg("plain"))) {
      server->send(400, "text/plain", "Invalid JSON body");
      return;
    }

    itemPath = PathUtils::urlDecode(body["from"].as<String>());
    destPath = PathUtils::urlDecode(body["to"].as<String>());
  }

  if (!PathUtils::isValidSdPath(itemPath) || !PathUtils::isValidSdPath(destPath)) {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  itemPath = PathUtils::normalizePath(itemPath);
  destPath = PathUtils::normalizePath(destPath);

  if (itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (pathContainsProtectedItem(itemPath) || pathContainsProtectedItem(destPath)) {
    server->send(403, "text/plain", "Cannot move protected items");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (itemName.startsWith(".")) {
    server->send(403, "text/plain", "Cannot move protected item");
    return;
  }

  bool itemExists = false;
  {
    SpiBusMutex::Guard guard;
    itemExists = Storage.exists(itemPath.c_str());
  }
  if (!itemExists) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file;
  {
    SpiBusMutex::Guard guard;
    file = Storage.open(itemPath.c_str());
  }
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  bool isDir = false;
  {
    SpiBusMutex::Guard guard;
    isDir = file.isDirectory();
    if (isDir) {
      file.close();
    }
  }
  if (isDir) {
    server->send(400, "text/plain", "Only files can be moved");
    return;
  }

  bool destExists = false;
  {
    SpiBusMutex::Guard guard;
    destExists = Storage.exists(destPath.c_str());
  }
  if (!destExists) {
    file.close();
    server->send(404, "text/plain", "Destination not found");
    return;
  }
  FsFile destDir;
  {
    SpiBusMutex::Guard guard;
    destDir = Storage.open(destPath.c_str());
  }
  if (!destDir || !destDir.isDirectory()) {
    if (destDir) destDir.close();
    file.close();
    server->send(400, "text/plain", "Destination is not a folder");
    return;
  }
  destDir.close();

  String newPath = destPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += itemName;

  if (newPath == itemPath) {
    file.close();
    server->send(200, "text/plain", "Already in destination");
    return;
  }
  bool targetExists = false;
  {
    SpiBusMutex::Guard guard;
    targetExists = Storage.exists(newPath.c_str());
  }
  if (targetExists) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  invalidateFeatureCachesIfNeeded(itemPath);
  bool success = false;
  {
    SpiBusMutex::Guard guard;
    success = file.rename(newPath.c_str());
  }
  file.close();

  if (success) {
    LOG_DBG("WEB", "Moved file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Moved successfully");
  } else {
    LOG_ERR("WEB", "Failed to move file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to move file");
  }
}

void CrossPointWebServer::handleDelete() const {
  if (!server->hasArg("paths") && !server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path(s)");
    return;
  }

  bool allSuccess = true;
  String failedItems;
  size_t processed = 0;

  auto processPath = [&](String itemPath) {
    itemPath = PathUtils::urlDecode(itemPath);
    if (!itemPath.startsWith("/")) {
      itemPath = "/" + itemPath;
    }

    if (!PathUtils::isValidSdPath(itemPath)) {
      failedItems += itemPath + " (invalid path); ";
      allSuccess = false;
      return;
    }

    itemPath = PathUtils::normalizePath(itemPath);
    if (itemPath.isEmpty() || itemPath == "/") {
      failedItems += itemPath + " (cannot delete root); ";
      allSuccess = false;
      return;
    }

    if (pathContainsProtectedItem(itemPath)) {
      failedItems += itemPath + " (protected path); ";
      allSuccess = false;
      return;
    }

    bool itemExists = false;
    {
      SpiBusMutex::Guard guard;
      itemExists = Storage.exists(itemPath.c_str());
    }
    if (!itemExists) {
      failedItems += itemPath + " (not found); ";
      allSuccess = false;
      return;
    }

    bool success = false;
    bool isDirectory = false;
    bool folderNotEmpty = false;
    {
      SpiBusMutex::Guard guard;
      FsFile file = Storage.open(itemPath.c_str());
      if (file && file.isDirectory()) {
        isDirectory = true;
        FsFile entry = file.openNextFile();
        if (entry) {
          entry.close();
          folderNotEmpty = true;
        }
        file.close();
        if (!folderNotEmpty) {
          success = Storage.rmdir(itemPath.c_str());
        }
      } else {
        if (file) {
          file.close();
        }
        success = Storage.remove(itemPath.c_str());
      }
    }

    if (folderNotEmpty) {
      failedItems += itemPath + " (folder not empty); ";
      allSuccess = false;
      return;
    }

    if (!success) {
      failedItems += itemPath + " (deletion failed); ";
      allSuccess = false;
      return;
    }

    invalidateFeatureCachesIfNeeded(itemPath);
    LOG_DBG("WEB", "Deleted %s: %s", isDirectory ? "folder" : "file", itemPath.c_str());
  };

  if (server->hasArg("paths")) {
    JsonDocument doc;
    const String pathsArg = server->arg("paths");
    const DeserializationError error = deserializeJson(doc, pathsArg);
    if (error) {
      server->send(400, "text/plain", "Invalid paths format");
      return;
    }

    JsonArray paths = doc.as<JsonArray>();
    if (paths.isNull() || paths.size() == 0) {
      server->send(400, "text/plain", "No paths provided");
      return;
    }

    for (const auto& p : paths) {
      processPath(p.as<String>());
      processed++;
    }
  } else {
    processPath(server->arg("path"));
    processed = 1;
  }

  if (processed == 0) {
    server->send(400, "text/plain", "No paths provided");
    return;
  }

  if (!allSuccess) {
    server->send(500, "text/plain", "Failed to delete some items: " + failedItems);
    return;
  }

  if (processed == 1) {
    server->send(200, "text/plain", "Deleted successfully");
  } else {
    server->send(200, "text/plain", "All items deleted successfully");
  }
}

void CrossPointWebServer::handleSettingsPage() const {
  sendPrecompressedHtml(server.get(), SettingsPageHtml, SettingsPageHtmlCompressedSize);
  LOG_DBG("WEB", "Served settings page");
}

void CrossPointWebServer::handlePokedexPluginPage() const {
  const auto payload = core::FeatureModules::getPokedexPluginPagePayload();
  if (!payload.available || payload.data == nullptr || payload.compressedSize == 0) {
    server->send(404, "text/plain", "Pokedex plugin not enabled in this build");
    return;
  }

  sendPrecompressedHtml(server.get(), payload.data, payload.compressedSize);
  LOG_DBG("WEB", "Served pokedex plugin page");
}

void CrossPointWebServer::handleWallpaperPluginPage() const {
  const auto payload = core::FeatureModules::getWallpaperPluginPagePayload();
  if (!payload.available || payload.data == nullptr || payload.compressedSize == 0) {
    server->send(404, "text/plain", "Wallpaper plugin not enabled in this build");
    return;
  }

  sendPrecompressedHtml(server.get(), payload.data, payload.compressedSize);
  LOG_DBG("WEB", "Served wallpaper plugin page");
}

void CrossPointWebServer::handleAnkiPluginPage() const {
  const auto payload = core::FeatureModules::getAnkiPluginPagePayload();
  if (!payload.available || payload.data == nullptr || payload.compressedSize == 0) {
    server->send(404, "text/plain", "Anki plugin not enabled in this build");
    return;
  }

  sendPrecompressedHtml(server.get(), payload.data, payload.compressedSize);
  LOG_DBG("WEB", "Served anki plugin page");
}

void CrossPointWebServer::handleGetSettings() const {
  const auto& settings = getSettingsList();

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  for (const auto& s : settings) {
    if (!s.key) continue;  // Skip ACTION-only entries

    doc.clear();
    doc["key"] = s.key;
    doc["name"] = I18N.get(s.nameId);
    doc["category"] = I18N.get(s.category);

    switch (s.type) {
      case SettingType::TOGGLE: {
        doc["type"] = "toggle";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        break;
      }
      case SettingType::ENUM: {
        doc["type"] = "enum";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        } else if (s.valueGetter) {
          doc["value"] = static_cast<int>(s.valueGetter());
        }
        JsonArray options = doc["options"].to<JsonArray>();
        if (s.dynamicValuesGetter) {
          for (const auto& opt : s.dynamicValuesGetter()) {
            options.add(opt.c_str());
          }
        } else {
          for (const auto& opt : s.enumValues) {
            options.add(I18N.get(opt));
          }
        }
        break;
      }
      case SettingType::VALUE: {
        doc["type"] = "value";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        doc["min"] = s.valueRange.min;
        doc["max"] = s.valueRange.max;
        doc["step"] = s.valueRange.step;
        break;
      }
      case SettingType::STRING: {
        doc["type"] = "string";
        const bool isPasswordField =
            s.key && (strstr(s.key, "password") != nullptr || strstr(s.key, "Password") != nullptr);
        if (isPasswordField) {
          // Do not expose stored passwords over the settings API.
          doc["value"] = "";
          if (s.stringGetter) {
            doc["hasValue"] = !s.stringGetter().empty();
          } else if (s.stringPtr) {
            doc["hasValue"] = s.stringPtr[0] != '\0';
          } else {
            doc["hasValue"] = false;
          }
        } else if (s.stringGetter) {
          doc["value"] = s.stringGetter();
        } else if (s.stringOffset > 0) {
          doc["value"] = reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset;
        }
        break;
      }
      default:
        continue;
    }

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) {
      LOG_DBG("WEB", "Skipping oversized setting JSON for: %s", s.key);
      continue;
    }

    if (seenFirst) {
      server->sendContent(",");
    } else {
      seenFirst = true;
    }
    server->sendContent(output);
  }

  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "Served settings API");
}

void CrossPointWebServer::handlePostSettings() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  const auto& settings = getSettingsList();
  int applied = 0;
  bool updatedKoreaderSettings = false;

  for (const auto& s : settings) {
    if (!s.key) continue;
    if (!doc[s.key].is<JsonVariant>()) continue;

    switch (s.type) {
      case SettingType::TOGGLE: {
        const int val = doc[s.key].as<int>() ? 1 : 0;
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = val;
        }
        applied++;
        updatedKoreaderSettings = updatedKoreaderSettings || (s.category == StrId::STR_KOREADER_SYNC);
        break;
      }
      case SettingType::ENUM: {
        const int val = doc[s.key].as<int>();
        const size_t optionCount = s.dynamicValuesGetter ? s.dynamicValuesGetter().size() : s.enumValues.size();
        if (val >= 0 && val < static_cast<int>(optionCount)) {
          if (s.valuePtr) {
            if (s.valuePtr == &CrossPointSettings::frontButtonLayout) {
              SETTINGS.applyFrontButtonLayoutPreset(static_cast<CrossPointSettings::FRONT_BUTTON_LAYOUT>(val));
            } else {
              SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
            }
          } else if (s.valueSetter) {
            s.valueSetter(static_cast<uint8_t>(val));
          }
          applied++;
          updatedKoreaderSettings = updatedKoreaderSettings || (s.category == StrId::STR_KOREADER_SYNC);
        }
        break;
      }
      case SettingType::VALUE: {
        const int val = doc[s.key].as<int>();
        if (val >= s.valueRange.min && val <= s.valueRange.max) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          }
          applied++;
          updatedKoreaderSettings = updatedKoreaderSettings || (s.category == StrId::STR_KOREADER_SYNC);
        }
        break;
      }
      case SettingType::STRING: {
        const std::string val = doc[s.key].as<std::string>();
        if (s.stringSetter) {
          s.stringSetter(val);
        } else if (s.stringOffset > 0 && s.stringMaxLen > 0) {
          char* ptr = reinterpret_cast<char*>(&SETTINGS) + s.stringOffset;
          strncpy(ptr, val.c_str(), s.stringMaxLen - 1);
          ptr[s.stringMaxLen - 1] = '\0';
        }
        applied++;
        updatedKoreaderSettings = updatedKoreaderSettings || (s.category == StrId::STR_KOREADER_SYNC);
        break;
      }
      default:
        break;
    }
  }

  core::FeatureModules::onWebSettingsApplied();
  if (updatedKoreaderSettings) {
    core::FeatureModules::saveKoreaderSettings();
  }

  SETTINGS.enforceButtonLayoutConstraints();
  if (!SETTINGS.saveToFile()) {
    LOG_WRN("WEB", "Failed to persist settings to SD card");
  }

  LOG_DBG("WEB", "Applied %d setting(s)", applied);
  server->send(200, "text/plain", String("Applied ") + String(applied) + " setting(s)");
}

void CrossPointWebServer::handleRescanUserFonts() {
  const auto result = core::FeatureModules::onFontScanRequested();
  if (!result.available) {
    server->send(404, "text/plain", "User font API disabled");
    return;
  }

  JsonDocument response;
  response["families"] = result.familyCount;
  response["activeLoaded"] = result.activeLoaded;

  String output;
  serializeJson(response, output);
  server->send(200, "application/json", output);
}

void CrossPointWebServer::handleFontUpload() {
  if (!core::FeatureModules::shouldRegisterWebRoute(core::WebOptionalRoute::UserFontsApi)) {
    uploadError = "User font API disabled";
    return;
  }

  esp_task_wdt_reset();

  if (!running || !server) return;

  const HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    esp_task_wdt_reset();
    uploadFileName = upload.filename;
    uploadSize = 0;
    uploadSuccess = false;
    uploadError = "";
    uploadBufferPos = 0;
    uploadStartTime = millis();
    totalWriteTime = 0;
    writeCount = 0;

    if (!PathUtils::isValidFilename(uploadFileName)) {
      uploadError = "Invalid filename";
      return;
    }
    if (isProtectedComponent(uploadFileName)) {
      uploadError = "Invalid filename";
      return;
    }

    String lowerName = uploadFileName;
    lowerName.toLowerCase();
    if (!lowerName.endsWith(".cpf")) {
      uploadError = "Only .cpf font files are accepted";
      return;
    }

    // Ensure /fonts directory exists
    {
      SpiBusMutex::Guard guard;
      if (!Storage.exists("/fonts")) {
        Storage.mkdir("/fonts");
      }
    }

    const String filePath = String("/fonts/") + uploadFileName;
    esp_task_wdt_reset();
    {
      SpiBusMutex::Guard guard;
      if (Storage.exists(filePath.c_str())) {
        Storage.remove(filePath.c_str());
      }
    }
    esp_task_wdt_reset();
    bool opened = false;
    {
      SpiBusMutex::Guard guard;
      opened = Storage.openFileForWrite("FONT", filePath, uploadFile);
    }
    if (!opened) {
      uploadError = "Failed to create font file on SD card";
    }
    esp_task_wdt_reset();

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile && uploadError.isEmpty()) {
      const uint8_t* data = upload.buf;
      size_t remaining = upload.currentSize;
      while (remaining > 0) {
        const size_t space = kUploadBufferSize - uploadBufferPos;
        const size_t toCopy = (remaining < space) ? remaining : space;
        memcpy(uploadBuffer + uploadBufferPos, data, toCopy);
        uploadBufferPos += toCopy;
        data += toCopy;
        remaining -= toCopy;
        if (uploadBufferPos >= kUploadBufferSize) {
          if (!flushUploadBuffer()) {
            uploadError = "Failed to write font data - disk may be full";
            SpiBusMutex::Guard guard;
            uploadFile.close();
            return;
          }
        }
      }
      uploadSize += upload.currentSize;
    }

  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      if (!flushUploadBuffer()) {
        uploadError = "Failed to write final font data";
      }
      {
        SpiBusMutex::Guard guard;
        uploadFile.close();
      }
      if (uploadError.isEmpty()) {
        uploadSuccess = true;
        LOG_DBG("WEB", "[FONT] Upload complete: %s (%d bytes)", uploadFileName.c_str(), uploadSize);
      }
    }

  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    uploadBufferPos = 0;
    if (uploadFile) {
      SpiBusMutex::Guard guard;
      uploadFile.close();
      Storage.remove((String("/fonts/") + uploadFileName).c_str());
    }
    uploadError = "Font upload aborted";
  }
}

void CrossPointWebServer::handleFontUploadPost() {
  if (!core::FeatureModules::shouldRegisterWebRoute(core::WebOptionalRoute::UserFontsApi)) {
    server->send(404, "text/plain", "User font API disabled");
    return;
  }

  if (!uploadSuccess) {
    const String error = uploadError.isEmpty() ? "Upload failed" : uploadError;
    server->send(400, "text/plain", error);
    return;
  }

  const auto result = core::FeatureModules::onFontScanRequested();

  JsonDocument response;
  response["ok"] = true;
  response["families"] = result.familyCount;

  String output;
  serializeJson(response, output);
  server->send(200, "application/json", output);
}

void CrossPointWebServer::handleRecentBooks() const {
  const auto& books = RECENT_BOOKS.getBooks();
  const bool includePokemon = core::FeatureModules::hasCapability(core::Capability::PokemonParty);

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  bool seenFirst = false;
  JsonDocument doc;

  for (const auto& book : books) {
    doc.clear();
    doc["path"] = book.path;
    doc["title"] = book.title;
    doc["author"] = book.author;
    doc["last_position"] = "";
    doc["last_opened"] = 0;
    doc["hasCover"] = !book.coverBmpPath.empty();

    BookProgressDataStore::ProgressData progress;
    if (BookProgressDataStore::loadProgress(book.path, progress)) {
      doc["last_position"] = BookProgressDataStore::formatPositionLabel(progress);
      appendProgressJson(doc["progress"].to<JsonObject>(), progress);
    } else {
      doc["progress"] = nullptr;
    }

    if (includePokemon) {
      JsonDocument pokemonDoc;
      if (PokemonBookDataStore::loadPokemonDocument(book.path, pokemonDoc) && pokemonDoc["pokemon"].is<JsonObject>()) {
        doc["pokemon"] = pokemonDoc["pokemon"];
      }
    }

    if (seenFirst) {
      server->sendContent(",");
    } else {
      seenFirst = true;
    }

    String output;
    serializeJson(doc, output);
    server->sendContent(output);
  }

  server->sendContent("]");
  server->sendContent("");
}

void CrossPointWebServer::handleGetBookProgress() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String bookPath = PathUtils::urlDecode(server->arg("path"));
  if (!PathUtils::isValidSdPath(bookPath)) {
    server->send(400, "text/plain", "Invalid path");
    return;
  }

  bookPath = PathUtils::normalizePath(bookPath);
  if (pathContainsProtectedItem(bookPath)) {
    server->send(403, "text/plain", "Cannot access protected items");
    return;
  }

  if (!Storage.exists(bookPath.c_str())) {
    server->send(404, "text/plain", "Book not found");
    return;
  }

  if (!BookProgressDataStore::supportsBookPath(bookPath.c_str())) {
    server->send(400, "text/plain", "Unsupported book type");
    return;
  }

  JsonDocument response;
  response["path"] = bookPath;

  BookProgressDataStore::ProgressData progress;
  if (BookProgressDataStore::loadProgress(bookPath.c_str(), progress)) {
    appendProgressJson(response["progress"].to<JsonObject>(), progress);
  } else {
    response["progress"] = nullptr;
  }

  String json;
  serializeJson(response, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleGetBookPokemon() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String bookPath = PathUtils::urlDecode(server->arg("path"));
  if (!PathUtils::isValidSdPath(bookPath)) {
    server->send(400, "text/plain", "Invalid path");
    return;
  }

  bookPath = PathUtils::normalizePath(bookPath);
  if (pathContainsProtectedItem(bookPath)) {
    server->send(403, "text/plain", "Cannot access protected items");
    return;
  }

  if (!Storage.exists(bookPath.c_str())) {
    server->send(404, "text/plain", "Book not found");
    return;
  }

  if (!PokemonBookDataStore::supportsBookPath(bookPath.c_str())) {
    server->send(400, "text/plain", "Unsupported book type");
    return;
  }

  JsonDocument response;
  response["path"] = bookPath;

  JsonDocument pokemonDoc;
  if (PokemonBookDataStore::loadPokemonDocument(bookPath.c_str(), pokemonDoc) &&
      pokemonDoc["pokemon"].is<JsonObject>()) {
    response["pokemon"] = pokemonDoc["pokemon"];
  } else {
    response["pokemon"] = nullptr;
  }

  String json;
  serializeJson(response, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handlePutBookPokemon() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing body");
    return;
  }

  JsonDocument request;
  if (deserializeJson(request, server->arg("plain"))) {
    server->send(400, "text/plain", "Invalid JSON body");
    return;
  }

  const String rawPath = request["path"] | "";
  if (rawPath.isEmpty()) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  if (!request["pokemon"].is<JsonObject>()) {
    server->send(400, "text/plain", "Missing pokemon object");
    return;
  }

  if (!PathUtils::isValidSdPath(rawPath)) {
    server->send(400, "text/plain", "Invalid path");
    return;
  }

  const String bookPath = PathUtils::normalizePath(rawPath);
  if (pathContainsProtectedItem(bookPath)) {
    server->send(403, "text/plain", "Cannot access protected items");
    return;
  }

  if (!Storage.exists(bookPath.c_str())) {
    server->send(404, "text/plain", "Book not found");
    return;
  }

  if (!PokemonBookDataStore::supportsBookPath(bookPath.c_str())) {
    server->send(400, "text/plain", "Unsupported book type");
    return;
  }

  if (!PokemonBookDataStore::savePokemonDocument(bookPath.c_str(), request["pokemon"])) {
    server->send(500, "text/plain", "Failed to save pokemon data");
    return;
  }

  JsonDocument response;
  response["ok"] = true;
  response["path"] = bookPath;
  response["pokemon"] = request["pokemon"];

  String json;
  serializeJson(response, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleDeleteBookPokemon() {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String bookPath = PathUtils::urlDecode(server->arg("path"));
  if (!PathUtils::isValidSdPath(bookPath)) {
    server->send(400, "text/plain", "Invalid path");
    return;
  }

  bookPath = PathUtils::normalizePath(bookPath);
  if (pathContainsProtectedItem(bookPath)) {
    server->send(403, "text/plain", "Cannot access protected items");
    return;
  }

  if (!Storage.exists(bookPath.c_str())) {
    server->send(404, "text/plain", "Book not found");
    return;
  }

  if (!PokemonBookDataStore::supportsBookPath(bookPath.c_str())) {
    server->send(400, "text/plain", "Unsupported book type");
    return;
  }

  if (!PokemonBookDataStore::deletePokemonDocument(bookPath.c_str())) {
    server->send(500, "text/plain", "Failed to delete pokemon data");
    return;
  }

  JsonDocument response;
  response["ok"] = true;
  response["path"] = bookPath;

  String json;
  serializeJson(response, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleCover() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  const String rawArg = server->arg("path");
  String bookPath = PathUtils::urlDecode(rawArg);

  if (!PathUtils::isValidSdPath(bookPath)) {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  bookPath = PathUtils::normalizePath(bookPath);
  if (pathContainsProtectedItem(bookPath)) {
    server->send(403, "text/plain", "Cannot access protected items");
    return;
  }

  // Look up the book in recent books to get the cached cover path
  const auto& books = RECENT_BOOKS.getBooks();
  std::string coverPath;

  for (const auto& book : books) {
    if (book.path == bookPath.c_str()) {
      coverPath = book.coverBmpPath;
      break;
    }
  }

  // If not found in recent books or no cover, try generating one
  if (coverPath.empty()) {
    core::FeatureModules::tryGetDocumentCoverPath(bookPath, coverPath);
  }

  if (coverPath.empty()) {
    server->send(404, "text/plain", "No cover available");
    return;
  }

  // Stream the BMP file to the client
  bool exists = false;
  {
    SpiBusMutex::Guard guard;
    exists = Storage.exists(coverPath.c_str());
  }
  if (!exists) {
    server->send(404, "text/plain", "Cover file not found");
    return;
  }

  FsFile file;
  {
    SpiBusMutex::Guard guard;
    file = Storage.open(coverPath.c_str());
  }
  if (!file) {
    server->send(500, "text/plain", "Failed to open cover");
    return;
  }

  size_t fileSize = 0;
  {
    SpiBusMutex::Guard guard;
    fileSize = file.size();
  }

  server->setContentLength(fileSize);
  server->sendHeader("Cache-Control", "public, max-age=3600");
  server->send(200, "image/bmp", "");

  WiFiClient client = server->client();
  uint8_t buffer[1024];
  while (true) {
    size_t bytesRead = 0;
    {
      SpiBusMutex::Guard guard;
      bytesRead = file.read(buffer, sizeof(buffer));
    }
    if (bytesRead == 0) break;
    client.write(buffer, bytesRead);
    yield();
    esp_task_wdt_reset();
  }
  {
    SpiBusMutex::Guard guard;
    file.close();
  }
}

void CrossPointWebServer::handleSleepImages() const {
#if ENABLE_IMAGE_SLEEP
  const char* ALLOWED_EXTS[] = {".bmp", ".png", ".jpg", ".jpeg"};
#else
  const char* ALLOWED_EXTS[] = {".bmp"};
#endif
  constexpr int NUM_ALLOWED = sizeof(ALLOWED_EXTS) / sizeof(ALLOWED_EXTS[0]);

  // Scan both /sleep (user-writable via web UI) and /.sleep (SD-card priority folder)
  const char* SLEEP_DIRS[] = {"/sleep", "/.sleep"};

  char output[300];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  for (const char* dirName : SLEEP_DIRS) {
    FsFile dir;
    {
      SpiBusMutex::Guard guard;
      dir = Storage.open(dirName);
    }
    if (!dir || !dir.isDirectory()) {
      if (dir) {
        SpiBusMutex::Guard guard;
        dir.close();
      }
      continue;
    }

    while (true) {
      std::string entryName;
      bool isEntryDir = false;
      bool done = false;

      {
        SpiBusMutex::Guard guard;
        FsFile file = dir.openNextFile();
        if (!file) {
          done = true;
        } else {
          char name[500];
          file.getName(name, sizeof(name));
          entryName = name;
          isEntryDir = file.isDirectory();
          file.close();
        }
      }

      if (done) break;
      if (isEntryDir) continue;
      if (entryName.empty() || entryName[0] == '.') continue;

      std::string lower = entryName;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      bool supported = false;
      for (int i = 0; i < NUM_ALLOWED; i++) {
        const size_t extLen = strlen(ALLOWED_EXTS[i]);
        if (lower.size() >= extLen && lower.substr(lower.size() - extLen) == ALLOWED_EXTS[i]) {
          supported = true;
          break;
        }
      }
      if (!supported) continue;

      doc.clear();
      doc["path"] = std::string(dirName) + "/" + entryName;
      doc["name"] = entryName;

      const size_t written = serializeJson(doc, output, outputSize);
      if (written >= outputSize) continue;

      if (seenFirst)
        server->sendContent(",");
      else
        seenFirst = true;
      server->sendContent(output);
    }

    {
      SpiBusMutex::Guard guard;
      dir.close();
    }
  }

  server->sendContent("]");
  server->sendContent("");
}

void CrossPointWebServer::handleSleepCoverGet() const {
  JsonDocument doc;
  doc["path"] = SETTINGS.sleepPinnedPath;
  const std::string p(SETTINGS.sleepPinnedPath);
  const size_t slash = p.find_last_of('/');
  doc["name"] = (slash == std::string::npos) ? p : p.substr(slash + 1);
  char buf[320];
  serializeJson(doc, buf, sizeof(buf));
  server->send(200, "application/json", buf);
}

void CrossPointWebServer::handleSleepCoverPin() {
  // Parse JSON body
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing body");
    return;
  }
  const String& body = server->arg("plain");
  JsonDocument reqDoc;
  if (deserializeJson(reqDoc, body)) {
    server->send(400, "text/plain", "Invalid JSON");
    return;
  }

  // --- Mode B: pin a book cover ---
  if (!reqDoc["bookPath"].isNull()) {
    const String rawBookPath = reqDoc["bookPath"].as<String>();
    if (!PathUtils::isValidSdPath(rawBookPath)) {
      server->send(400, "text/plain", "Invalid bookPath");
      return;
    }
    const String bookPath = PathUtils::normalizePath(rawBookPath);

    // Resolve cover BMP
    std::string coverPath;
    const auto& books = RECENT_BOOKS.getBooks();
    for (const auto& book : books) {
      if (book.path == bookPath.c_str()) {
        coverPath = book.coverBmpPath;
        break;
      }
    }
    if (coverPath.empty()) {
      core::FeatureModules::tryGetDocumentCoverPath(bookPath, coverPath);
    }
    if (coverPath.empty()) {
      server->send(404, "text/plain", "No cover available for this book");
      return;
    }

    // Ensure /sleep/ directory exists
    {
      SpiBusMutex::Guard guard;
      if (!Storage.exists("/sleep")) {
        Storage.mkdir("/sleep");
      }
    }

    // Copy cover BMP to /sleep/.pinned-cover.bmp
    constexpr const char* kPinnedDest = "/sleep/.pinned-cover.bmp";
    bool copyOk = false;
    {
      SpiBusMutex::Guard guard;
      FsFile src = Storage.open(coverPath.c_str());
      if (src) {
        FsFile dst = Storage.open(kPinnedDest, O_WRONLY | O_CREAT | O_TRUNC);
        if (dst) {
          uint8_t buf[512];
          size_t n;
          while ((n = src.read(buf, sizeof(buf))) > 0) {
            dst.write(buf, n);
          }
          dst.close();
          copyOk = true;
        }
        src.close();
      }
    }
    if (!copyOk) {
      server->send(500, "text/plain", "Failed to copy cover");
      return;
    }

    strncpy(SETTINGS.sleepPinnedPath, kPinnedDest, sizeof(SETTINGS.sleepPinnedPath) - 1);
    SETTINGS.sleepPinnedPath[sizeof(SETTINGS.sleepPinnedPath) - 1] = '\0';
    bool saved = false;
    {
      SpiBusMutex::Guard guard;
      saved = SETTINGS.saveToFile();
    }
    if (!saved) {
      server->send(500, "text/plain", "Failed to save settings");
      return;
    }

    JsonDocument respDoc;
    respDoc["pinnedPath"] = SETTINGS.sleepPinnedPath;
    char respBuf[300];
    serializeJson(respDoc, respBuf, sizeof(respBuf));
    server->send(200, "application/json", respBuf);
    return;
  }

  // --- Mode A: pin a sleep-folder image (or clear) ---
  const String rawPath = reqDoc["path"].as<String>();

  if (rawPath.isEmpty()) {
    // Clear pin
    SETTINGS.sleepPinnedPath[0] = '\0';
    bool saved = false;
    {
      SpiBusMutex::Guard guard;
      saved = SETTINGS.saveToFile();
    }
    server->send(saved ? 200 : 500, "text/plain", saved ? "Cleared" : "Failed to save");
    return;
  }

  if (!PathUtils::isValidSdPath(rawPath)) {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  const String pinnedPath = PathUtils::normalizePath(rawPath);

  bool exists = false;
  {
    SpiBusMutex::Guard guard;
    exists = Storage.exists(pinnedPath.c_str());
  }
  if (!exists) {
    server->send(404, "text/plain", "File not found");
    return;
  }

  strncpy(SETTINGS.sleepPinnedPath, pinnedPath.c_str(), sizeof(SETTINGS.sleepPinnedPath) - 1);
  SETTINGS.sleepPinnedPath[sizeof(SETTINGS.sleepPinnedPath) - 1] = '\0';
  bool saved = false;
  {
    SpiBusMutex::Guard guard;
    saved = SETTINGS.saveToFile();
  }

  JsonDocument respDoc;
  respDoc["pinnedPath"] = SETTINGS.sleepPinnedPath;
  char respBuf[300];
  serializeJson(respDoc, respBuf, sizeof(respBuf));
  server->send(saved ? 200 : 500, "application/json", respBuf);
}

void CrossPointWebServer::handleOpenBook() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }
  JsonDocument doc;
  const auto err = deserializeJson(doc, server->arg("plain"));
  if (err) {
    server->send(400, "text/plain", "Invalid JSON");
    return;
  }
  const char* path = doc["path"] | "";
  if (!path || path[0] == '\0') {
    server->send(400, "text/plain", "Missing path");
    return;
  }
  if (!PathUtils::isValidSdPath(String(path))) {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  bool exists = false;
  {
    SpiBusMutex::Guard guard;
    exists = Storage.exists(path);
  }
  if (!exists) {
    server->send(404, "text/plain", "File not found");
    return;
  }
  APP_STATE.pendingOpenPath = path;
  server->send(202, "application/json", "{\"status\":\"opening\"}");
}

void CrossPointWebServer::handleRemoteButton() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server->arg("plain"))) {
    server->send(400, "text/plain", "Invalid JSON");
    return;
  }
  const char* btn = doc["button"] | "";
  int8_t pageTurn = 0;
  if (strcmp(btn, "page_forward") == 0 || strcmp(btn, "next") == 0) {
    pageTurn = 1;
  } else if (strcmp(btn, "page_back") == 0 || strcmp(btn, "prev") == 0 || strcmp(btn, "previous") == 0) {
    pageTurn = -1;
  } else {
    server->send(400, "text/plain", "Unknown button; use page_forward or page_back");
    return;
  }
  APP_STATE.pendingPageTurn = pageTurn;
  server->send(202, "application/json", "{\"status\":\"ok\"}");
}

void CrossPointWebServer::handleGetSettingsRaw() const {
  const CrossPointSettings& s = SETTINGS;
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
  doc["hideBatteryPercentage"] = s.hideBatteryPercentage;
  doc["longPressChapterSkip"] = s.longPressChapterSkip;
  doc["hyphenationEnabled"] = s.hyphenationEnabled;
  doc["backgroundServerOnCharge"] = s.backgroundServerOnCharge;
  doc["todoFallbackCover"] = s.todoFallbackCover;
  doc["timeMode"] = s.timeMode;
  doc["timeZoneOffset"] = s.timeZoneOffset;
  doc["releaseChannel"] = s.releaseChannel;
  doc["uiTheme"] = s.uiTheme;
  doc["fadingFix"] = s.fadingFix;
  doc["darkMode"] = s.darkMode;
  doc["embeddedStyle"] = s.embeddedStyle;
  doc["usbMscPromptOnConnect"] = s.usbMscPromptOnConnect;
  doc["userFontPath"] = s.userFontPath;
  doc["selectedOtaBundle"] = s.selectedOtaBundle;
  doc["installedOtaBundle"] = s.installedOtaBundle;
  doc["deviceName"] = s.deviceName;
  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleWifiStatus() const {
  if (!core::FeatureModules::shouldRegisterWebRoute(core::WebOptionalRoute::WebWifiSetupApi)) {
    server->send(404, "text/plain", "WiFi setup API disabled");
    return;
  }
  JsonDocument doc;
  const wl_status_t wifiSt = WiFi.status();
  const bool connected = wifiSt == WL_CONNECTED;
  doc["connected"] = connected;
  if (apMode) {
    doc["mode"] = "AP";
    doc["ssid"] = WiFi.softAPSSID();
  } else if (connected) {
    doc["mode"] = "STA";
    doc["ssid"] = WiFi.SSID();
    doc["ip"] = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();
  } else {
    doc["mode"] = "STA";
    const char* stStr = (wifiSt == WL_CONNECT_FAILED)  ? "failed"
                        : (wifiSt == WL_NO_SSID_AVAIL) ? "no_ssid"
                        : (wifiSt == WL_IDLE_STATUS)   ? "connecting"
                                                       : "disconnected";
    doc["status"] = stStr;
  }
  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

// WebSocket callback trampoline
void CrossPointWebServer::wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (wsInstance) {
    wsInstance->onWebSocketEvent(num, type, payload, length);
  }
}

// WebSocket event handler for fast binary uploads
// Protocol:
//   1. Client sends TEXT message: "START:<filename>:<size>:<path>"
//   2. Client sends BINARY messages with file data chunks
//   3. Server sends TEXT "PROGRESS:<received>:<total>" after each chunk
//   4. Server sends TEXT "DONE" or "ERROR:<message>" when complete
void CrossPointWebServer::onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  auto buildWsUploadFilePath = [this]() {
    String filePath = wsUploadPath;
    if (!filePath.endsWith("/")) {
      filePath += "/";
    }
    filePath += wsUploadFileName;
    return filePath;
  };

  auto cleanupPartialWsUpload = [this, &buildWsUploadFilePath](const char* reason) {
    const bool hasPath = !wsUploadPath.isEmpty() && !wsUploadFileName.isEmpty();
    const String filePath = hasPath ? buildWsUploadFilePath() : String();
    {
      SpiBusMutex::Guard guard;
      if (wsUploadFile) {
        wsUploadFile.close();
      }
      if (hasPath) {
        Storage.remove(filePath.c_str());
      }
    }
    if (hasPath) {
      LOG_DBG("WS", "Deleted incomplete upload (%s): %s", reason, filePath.c_str());
    }
  };

  auto resetWsUploadState = [this]() {
    wsUploadInProgress = false;
    wsUploadFileName.clear();
    wsUploadPath.clear();
    wsUploadSize = 0;
    wsUploadReceived = 0;
    wsLastProgressSent = 0;
    wsUploadStartTime = 0;
    wsUploadOwnerClient = 0;
    wsUploadOwnerValid = false;
  };

  switch (type) {
    case WStype_DISCONNECTED:
      LOG_DBG("WS", "Client %u disconnected", num);
      if ((wsUploadInProgress || wsUploadFile) && wsUploadOwnerValid && wsUploadOwnerClient == num) {
        cleanupPartialWsUpload("owner disconnect");
        resetWsUploadState();
      } else if (wsUploadInProgress || wsUploadFile) {
        LOG_DBG("WS", "Ignoring disconnect from non-owner client %u during upload", num);
      }
      break;

    case WStype_CONNECTED: {
      LOG_DBG("WS", "Client %u connected", num);
      break;
    }

    case WStype_TEXT: {
      if ((wsUploadInProgress || wsUploadFile) && wsUploadOwnerValid && wsUploadOwnerClient != num) {
        wsServer->sendTXT(num, "ERROR:Upload in progress by another client");
        return;
      }

      if (!payload) {
        wsServer->sendTXT(num, "ERROR:Missing control payload");
        return;
      }
      if (length == 0 || length > WS_CONTROL_MESSAGE_MAX_BYTES) {
        wsServer->sendTXT(num, "ERROR:Control message too large");
        return;
      }

      String msg;
      msg.reserve(length);
      for (size_t i = 0; i < length; i++) {
        const char c = static_cast<char>(payload[i]);
        if (c == '\0') {
          wsServer->sendTXT(num, "ERROR:Invalid control payload");
          return;
        }
        msg += c;
      }
      LOG_DBG("WS", "Text from client %u: %s", num, msg.c_str());

      // Remote page-turn commands — handled before upload guard so they work during idle.
      if (msg.equalsIgnoreCase("PAGE:NEXT") || msg.equalsIgnoreCase("PAGE:FORWARD")) {
        APP_STATE.pendingPageTurn = 1;
        wsServer->sendTXT(num, "OK");
        return;
      }
      if (msg.equalsIgnoreCase("PAGE:PREV") || msg.equalsIgnoreCase("PAGE:BACK")) {
        APP_STATE.pendingPageTurn = -1;
        wsServer->sendTXT(num, "OK");
        return;
      }

      if (!msg.startsWith("START:")) {
        wsServer->sendTXT(num, "ERROR:Unknown command");
        return;
      }

      // Parse: START:<filename>:<size>:<path> (filename/path URL-encoded)
      const int firstColon = msg.indexOf(':', 6);
      const int secondColon = firstColon > 0 ? msg.indexOf(':', firstColon + 1) : -1;
      if (firstColon <= 0 || secondColon <= 0) {
        wsServer->sendTXT(num, "ERROR:Invalid START format");
        return;
      }

      String requestedFileName = PathUtils::urlDecode(msg.substring(6, firstColon));
      String requestedPath = PathUtils::urlDecode(msg.substring(secondColon + 1));
      size_t requestedSize = 0;
      if (!parseStrictSize(msg.substring(firstColon + 1, secondColon), requestedSize)) {
        wsServer->sendTXT(num, "ERROR:Invalid size (1..512MB)");
        return;
      }
      if (requestedSize > WS_UPLOAD_MAX_BYTES) {
        LOG_WRN("WS", "Rejected upload with declared size %u bytes (max %u)", static_cast<unsigned int>(requestedSize),
                static_cast<unsigned int>(WS_UPLOAD_MAX_BYTES));
        wsServer->sendTXT(num, "ERROR:Declared size exceeds limit");
        return;
      }

      // Validate filename against traversal attacks
      if (!PathUtils::isValidFilename(requestedFileName)) {
        LOG_WRN("WS", "Invalid filename rejected: %s", requestedFileName.c_str());
        wsServer->sendTXT(num, "ERROR:Invalid filename");
        return;
      }
      if (isProtectedComponent(requestedFileName)) {
        LOG_WRN("WS", "Protected filename rejected: %s", requestedFileName.c_str());
        wsServer->sendTXT(num, "ERROR:Protected filename");
        return;
      }

      // Validate path against traversal attacks
      if (!PathUtils::isValidSdPath(requestedPath)) {
        LOG_WRN("WS", "Path validation failed: %s", requestedPath.c_str());
        wsServer->sendTXT(num, "ERROR:Invalid path");
        return;
      }

      requestedPath = PathUtils::normalizePath(requestedPath);
      if (pathContainsProtectedItem(requestedPath)) {
        LOG_WRN("WS", "Protected path rejected: %s", requestedPath.c_str());
        wsServer->sendTXT(num, "ERROR:Protected path");
        return;
      }

      if (wsUploadInProgress || wsUploadFile) {
        cleanupPartialWsUpload("superseded by owner");
        resetWsUploadState();
      }

      wsUploadFileName = requestedFileName;
      wsUploadPath = requestedPath;
      wsUploadSize = requestedSize;
      wsUploadReceived = 0;
      wsLastProgressSent = 0;
      wsUploadStartTime = millis();
      wsUploadOwnerClient = num;
      wsUploadOwnerValid = true;

      const String filePath = buildWsUploadFilePath();
      LOG_DBG("WS", "Starting upload: %s (%u bytes) to %s", wsUploadFileName.c_str(),
              static_cast<unsigned int>(wsUploadSize), filePath.c_str());

      // Check if file exists and remove it
      esp_task_wdt_reset();
      {
        SpiBusMutex::Guard guard;
        if (Storage.exists(filePath.c_str())) {
          Storage.remove(filePath.c_str());
        }
      }

      // Open file for writing
      esp_task_wdt_reset();
      bool fileOpened = false;
      {
        SpiBusMutex::Guard guard;
        fileOpened = Storage.openFileForWrite("WS", filePath, wsUploadFile);
      }
      if (!fileOpened) {
        wsServer->sendTXT(num, "ERROR:Failed to create file");
        resetWsUploadState();
        return;
      }
      esp_task_wdt_reset();

      wsUploadInProgress = true;
      wsServer->sendTXT(num, "READY");
      break;
    }

    case WStype_BIN: {
      if (!wsUploadInProgress || !wsUploadFile) {
        wsServer->sendTXT(num, "ERROR:No upload in progress");
        return;
      }
      if (!wsUploadOwnerValid || wsUploadOwnerClient != num) {
        wsServer->sendTXT(num, "ERROR:Upload in progress by another client");
        return;
      }
      if (!payload) {
        cleanupPartialWsUpload("missing chunk payload");
        resetWsUploadState();
        wsServer->sendTXT(num, "ERROR:Missing chunk payload");
        return;
      }
      if (length == 0) {
        wsServer->sendTXT(num, "ERROR:Empty chunk");
        return;
      }
      if (wsUploadReceived > wsUploadSize || length > (wsUploadSize - wsUploadReceived)) {
        cleanupPartialWsUpload("oversize chunk");
        resetWsUploadState();
        wsServer->sendTXT(num, "ERROR:Chunk exceeds declared size");
        return;
      }

      // Write binary data directly to file
      esp_task_wdt_reset();
      size_t written = 0;
      {
        SpiBusMutex::Guard guard;
        written = wsUploadFile.write(payload, length);
      }
      esp_task_wdt_reset();

      if (written != length) {
        cleanupPartialWsUpload("write failure");
        resetWsUploadState();
        wsServer->sendTXT(num, "ERROR:Write failed - disk full?");
        return;
      }

      wsUploadReceived += written;

      // Send progress update (every 64KB or at end)
      if (wsUploadReceived - wsLastProgressSent >= 65536 || wsUploadReceived >= wsUploadSize) {
        String progress = "PROGRESS:" + String(wsUploadReceived) + ":" + String(wsUploadSize);
        wsServer->sendTXT(num, progress);
        wsLastProgressSent = wsUploadReceived;
      }

      // Check if upload complete
      if (wsUploadReceived == wsUploadSize) {
        {
          SpiBusMutex::Guard guard;
          wsUploadFile.close();
        }
        wsUploadInProgress = false;

        wsLastCompleteName = wsUploadFileName;
        wsLastCompleteSize = wsUploadSize;
        wsLastCompleteAt = millis();

        unsigned long elapsed = millis() - wsUploadStartTime;
        float kbps = (elapsed > 0) ? (wsUploadSize / 1024.0) / (elapsed / 1000.0) : 0;

        LOG_DBG("WS", "Upload complete: %s (%u bytes in %lu ms, %.1f KB/s)", wsUploadFileName.c_str(),
                static_cast<unsigned int>(wsUploadSize), elapsed, kbps);

        // Clear caches to prevent stale data when overwriting files
        String filePath = wsUploadPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += wsUploadFileName;
        invalidateFeatureCachesIfNeeded(filePath);

        wsServer->sendTXT(num, "DONE");
        wsLastProgressSent = 0;
        wsUploadOwnerClient = 0;
        wsUploadOwnerValid = false;
      }
      break;
    }

    default:
      break;
  }
}

#include "WifiCredentialStore.h"

void CrossPointWebServer::handleWifiScan() const {
  if (!core::FeatureModules::shouldRegisterWebRoute(core::WebOptionalRoute::WebWifiSetupApi)) {
    server->send(404, "text/plain", "WiFi setup API disabled");
    return;
  }

  int n = WiFi.scanNetworks();
  const bool staConnected = (WiFi.getMode() & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  const String activeSsid = staConnected ? WiFi.SSID() : String();
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();

  for (int i = 0; i < n; ++i) {
    const String networkSsid = WiFi.SSID(i);
    const bool encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    JsonObject obj = array.add<JsonObject>();
    obj["ssid"] = networkSsid;
    obj["rssi"] = WiFi.RSSI(i);
    obj["encrypted"] = encrypted;
    obj["secured"] = encrypted;
    obj["saved"] = WIFI_STORE.hasSavedCredential(networkSsid.c_str());
    obj["connected"] = staConnected && (networkSsid == activeSsid);
  }
  WiFi.scanDelete();

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleWifiConnect() const {
  if (!core::FeatureModules::shouldRegisterWebRoute(core::WebOptionalRoute::WebWifiSetupApi)) {
    server->send(404, "text/plain", "WiFi setup API disabled");
    return;
  }

  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing body");
    return;
  }
  JsonDocument doc;
  deserializeJson(doc, server->arg("plain"));

  String ssid = doc["ssid"];
  String password = doc["password"];

  if (ssid.length() == 0) {
    server->send(400, "text/plain", "SSID required");
    return;
  }

  WIFI_STORE.addCredential(ssid.c_str(), password.c_str());
  WIFI_STORE.saveToFile();

  server->send(200, "text/plain", "WiFi credentials saved");
}

void CrossPointWebServer::handleWifiForget() const {
  if (!core::FeatureModules::shouldRegisterWebRoute(core::WebOptionalRoute::WebWifiSetupApi)) {
    server->send(404, "text/plain", "WiFi setup API disabled");
    return;
  }

  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing body");
    return;
  }
  JsonDocument doc;
  deserializeJson(doc, server->arg("plain"));
  String ssid = doc["ssid"];

  if (ssid.length() > 0) {
    WIFI_STORE.removeCredential(ssid.c_str());
    WIFI_STORE.saveToFile();
    server->send(200, "text/plain", "WiFi credentials removed");
  } else {
    server->send(400, "text/plain", "SSID required");
  }
}

void CrossPointWebServer::handleOtaCheckPost() {
  if (!core::FeatureModules::shouldRegisterWebRoute(core::WebOptionalRoute::OtaApi)) {
    server->send(404, "application/json", "{\"status\":\"error\",\"message\":\"OTA API disabled\"}");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    server->send(503, "application/json", "{\"status\":\"error\",\"message\":\"Not connected to WiFi\"}");
    return;
  }

  switch (core::FeatureModules::startOtaWebCheck()) {
    case core::FeatureModules::OtaWebStartResult::AlreadyChecking:
      server->send(200, "application/json", "{\"status\":\"checking\"}");
      return;
    case core::FeatureModules::OtaWebStartResult::Started:
      server->send(202, "application/json", "{\"status\":\"checking\"}");
      return;
    case core::FeatureModules::OtaWebStartResult::StartTaskFailed:
      server->send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to start task\"}");
      return;
    case core::FeatureModules::OtaWebStartResult::Disabled:
      server->send(404, "application/json", "{\"status\":\"error\",\"message\":\"OTA API disabled\"}");
      return;
  }
}

void CrossPointWebServer::handleOtaCheckGet() const {
  if (!core::FeatureModules::shouldRegisterWebRoute(core::WebOptionalRoute::OtaApi)) {
    server->send(404, "application/json", "{\"status\":\"error\",\"message\":\"OTA API disabled\"}");
    return;
  }

  const core::FeatureModules::OtaWebCheckSnapshot status = core::FeatureModules::getOtaWebCheckSnapshot();
  if (status.status == core::FeatureModules::OtaWebCheckStatus::Disabled) {
    server->send(404, "application/json", "{\"status\":\"error\",\"message\":\"OTA API disabled\"}");
    return;
  }

  JsonDocument doc;
  doc["currentVersion"] = CROSSPOINT_VERSION;

  if (status.status == core::FeatureModules::OtaWebCheckStatus::Checking) {
    doc["status"] = "checking";
  } else if (status.status == core::FeatureModules::OtaWebCheckStatus::Done) {
    doc["status"] = "done";
    doc["available"] = status.available;
    doc["latestVersion"] = status.latestVersion.c_str();
    doc["latest_version"] = status.latestVersion.c_str();
    doc["message"] = status.message.c_str();
    doc["errorCode"] = status.errorCode;
    doc["error_code"] = status.errorCode;
  } else {
    doc["status"] = "idle";
  }

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleAnkiGetCards() const {
  if (!core::FeatureModules::isEnabled("anki_support")) {
    server->send(404, "text/plain", "Anki support disabled");
    return;
  }

  // buildCardsJson holds the AnkiStore mutex internally; release before send().
  std::string json;
  util::AnkiStore::getInstance().buildCardsJson(json);
  server->send(200, "application/json", json.c_str());
}

void CrossPointWebServer::handleAnkiClearCards() {
  if (!core::FeatureModules::isEnabled("anki_support")) {
    server->send(404, "text/plain", "Anki support disabled");
    return;
  }

  util::AnkiStore::getInstance().clear();
  util::AnkiStore::getInstance().save();
  server->send(200, "application/json", "{\"status\":\"ok\"}");
}
