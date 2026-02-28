#include "CrossPointWebServer.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <map>

#include "CrossPointSettings.h"
#include "SettingsList.h"
#include "WebDAVHandler.h"
#include "WifiCredentialStore.h"
#include <FontManager.h>
#include "html/FilesPageHtml.generated.h"
#include "html/HomePageHtml.generated.h"
#include "html/SettingsPageHtml.generated.h"
#include "util/StringUtils.h"

namespace {
// Folders/files to hide from the web interface file browser
// Note: Items starting with "." are automatically hidden
const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
constexpr size_t HIDDEN_ITEMS_COUNT = sizeof(HIDDEN_ITEMS) / sizeof(HIDDEN_ITEMS[0]);
constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
constexpr uint16_t LOCAL_UDP_PORT = 8134;

// Static pointer for WebSocket callback (WebSocketsServer requires C-style callback)
CrossPointWebServer* wsInstance = nullptr;

// WebSocket upload state
FsFile wsUploadFile;
String wsUploadFileName;
String wsUploadPath;
size_t wsUploadSize = 0;
size_t wsUploadReceived = 0;
unsigned long wsUploadStartTime = 0;
bool wsUploadInProgress = false;
String wsLastCompleteName;
size_t wsLastCompleteSize = 0;
unsigned long wsLastCompleteAt = 0;

// Helper function to clear epub cache after upload
void clearEpubCacheIfNeeded(const String& filePath) {
  // Only clear cache for .epub files
  if (StringUtils::checkFileExtension(filePath, ".epub")) {
    Epub(filePath.c_str(), "/.crosspoint").clearCache();
    LOG_DBG("WEB", "Cleared epub cache for: %s", filePath.c_str());
  }
}

String normalizeWebPath(const String& inputPath) {
  if (inputPath.isEmpty() || inputPath == "/") {
    return "/";
  }
  std::string normalized = FsHelpers::normalisePath(inputPath.c_str());
  String result = normalized.c_str();
  if (result.isEmpty()) {
    return "/";
  }
  if (!result.startsWith("/")) {
    result = "/" + result;
  }
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }
  return result;
}

bool isProtectedItemName(const String& name) {
  if (name.startsWith(".")) {
    return true;
  }
  for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
    if (name.equals(HIDDEN_ITEMS[i])) {
      return true;
    }
  }
  return false;
}

bool isReaderFontFamilySetting(const SettingInfo& s) {
  return s.key != nullptr && strcmp(s.key, "fontFamily") == 0;
}

bool isUiFontFamilyKey(const char* key) {
  return key != nullptr && strcmp(key, "uiFontFamily") == 0;
}

bool isLanguageSettingKey(const char* key) {
  return key != nullptr && strcmp(key, "language") == 0;
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
  server->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
  server->on("/download", HTTP_GET, [this] { handleDownload(); });

  // Upload endpoint with special handling for multipart form data
  server->on("/upload", HTTP_POST, [this] { handleUploadPost(upload); }, [this] { handleUpload(upload); });

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
  server->on("/api/settings", HTTP_GET, [this] { handleGetSettings(); });
  server->on("/api/settings", HTTP_POST, [this] { handlePostSettings(); });

  // WiFi credential management endpoints (CJK)
  server->on("/api/wifi/scan", HTTP_GET, [this] { handleWifiScan(); });
  server->on("/api/wifi/save", HTTP_POST, [this] { handleWifiSave(); });
  server->on("/api/wifi/list", HTTP_GET, [this] { handleWifiList(); });
  server->on("/api/wifi/delete", HTTP_POST, [this] { handleWifiDelete(); });

  server->onNotFound([this] { handleNotFound(); });
  LOG_DBG("WEB", "[MEM] Free heap after route setup: %d bytes", ESP.getFreeHeap());

  // Collect WebDAV headers and register handler
  const char* davHeaders[] = {"Depth", "Destination", "Overwrite", "If", "Lock-Token", "Timeout"};
  server->collectHeaders(davHeaders, 6);
  server->addHandler(new WebDAVHandler());  // Note: WebDAVHandler will be deleted by WebServer when server is stopped
  LOG_DBG("WEB", "WebDAV handler initialized");

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

  // Note: Static upload variables (uploadFileName, uploadPath, uploadError) are declared
  // later in the file and will be cleared when they go out of scope or on next upload
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

static void sendHtmlContent(WebServer* server, const char* data, size_t len) {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "text/html", data, len);
}

void CrossPointWebServer::handleRoot() const {
  sendHtmlContent(server.get(), HomePageHtml, sizeof(HomePageHtml));
  LOG_DBG("WEB", "Served root page");
}

void CrossPointWebServer::handleNotFound() const {
  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

void CrossPointWebServer::handleStatus() const {
  // Get correct IP based on AP vs STA mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  JsonDocument doc;
  doc["version"] = CROSSPOINT_VERSION;
  doc["ip"] = ipAddr;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["rssi"] = apMode ? 0 : WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::scanFiles(const char* path, const std::function<void(FileInfo)>& callback) const {
  FsFile root = Storage.open(path);
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

  FsFile file = root.openNextFile();
  char name[500];
  while (file) {
    file.getName(name, sizeof(name));
    auto fileName = String(name);

    // Skip hidden items (starting with ".")
    bool shouldHide = fileName.startsWith(".");

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
      FileInfo info;
      info.name = fileName;
      info.isDirectory = file.isDirectory();

      if (info.isDirectory) {
        info.size = 0;
        info.isEpub = false;
      } else {
        info.size = file.size();
        info.isEpub = isEpubFile(info.name);
      }

      callback(info);
    }

    file.close();
    yield();               // Yield to allow WiFi and other tasks to process during long scans
    esp_task_wdt_reset();  // Reset watchdog to prevent timeout on large directories
    file = root.openNextFile();
  }
  root.close();
}

bool CrossPointWebServer::isEpubFile(const String& filename) const {
  String lower = filename;
  lower.toLowerCase();
  return lower.endsWith(".epub");
}

void CrossPointWebServer::handleFileList() const {
  sendHtmlContent(server.get(), FilesPageHtml, sizeof(FilesPageHtml));
}

void CrossPointWebServer::handleFileListData() const {
  // Get current path from query string (default to root)
  String currentPath = "/";
  if (server->hasArg("path")) {
    currentPath = server->arg("path");
    // Ensure path starts with /
    if (!currentPath.startsWith("/")) {
      currentPath = "/" + currentPath;
    }
    // Remove trailing slash unless it's root
    if (currentPath.length() > 1 && currentPath.endsWith("/")) {
      currentPath = currentPath.substring(0, currentPath.length() - 1);
    }
  }

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");
  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  scanFiles(currentPath.c_str(), [this, &output, &doc, seenFirst](const FileInfo& info) mutable {
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
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = server->arg("path");
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (!itemPath.startsWith("/")) {
    itemPath = "/" + itemPath;
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

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Path is a directory");
    return;
  }

  String contentType = "application/octet-stream";
  if (isEpubFile(itemPath)) {
    contentType = "application/epub+zip";
  }

  char nameBuf[128] = {0};
  String filename = "download";
  if (file.getName(nameBuf, sizeof(nameBuf))) {
    filename = nameBuf;
  }

  server->setContentLength(file.size());
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server->send(200, contentType.c_str(), "");

  NetworkClient client = server->client();
  client.write(file);
  file.close();
}

// Diagnostic counters for upload performance analysis
static unsigned long uploadStartTime = 0;
static unsigned long totalWriteTime = 0;
static size_t writeCount = 0;

static bool flushUploadBuffer(CrossPointWebServer::UploadState& state) {
  if (state.bufferPos > 0 && state.file) {
    esp_task_wdt_reset();  // Reset watchdog before potentially slow SD write
    const unsigned long writeStart = millis();
    const size_t written = state.file.write(state.buffer.data(), state.bufferPos);
    totalWriteTime += millis() - writeStart;
    writeCount++;
    esp_task_wdt_reset();  // Reset watchdog after SD write

    if (written != state.bufferPos) {
      LOG_DBG("WEB", "[UPLOAD] Buffer flush failed: expected %d, wrote %d", state.bufferPos, written);
      state.bufferPos = 0;
      return false;
    }
    state.bufferPos = 0;
  }
  return true;
}

void CrossPointWebServer::handleUpload(UploadState& state) const {
  static size_t lastLoggedSize = 0;

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

    state.fileName = upload.filename;
    state.size = 0;
    state.success = false;
    state.error = "";
    uploadStartTime = millis();
    lastLoggedSize = 0;
    state.bufferPos = 0;
    totalWriteTime = 0;
    writeCount = 0;

    // Get upload path from query parameter (defaults to root if not specified)
    // Note: We use query parameter instead of form data because multipart form
    // fields aren't available until after file upload completes
    if (server->hasArg("path")) {
      state.path = server->arg("path");
      // Ensure path starts with /
      if (!state.path.startsWith("/")) {
        state.path = "/" + state.path;
      }
      // Remove trailing slash unless it's root
      if (state.path.length() > 1 && state.path.endsWith("/")) {
        state.path = state.path.substring(0, state.path.length() - 1);
      }
    } else {
      state.path = "/";
    }

    LOG_DBG("WEB", "[UPLOAD] START: %s to path: %s", state.fileName.c_str(), state.path.c_str());
    LOG_DBG("WEB", "[UPLOAD] Free heap: %d bytes", ESP.getFreeHeap());

    // Create file path
    String filePath = state.path;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += state.fileName;

    // Check if file already exists - SD operations can be slow
    esp_task_wdt_reset();
    if (Storage.exists(filePath.c_str())) {
      LOG_DBG("WEB", "[UPLOAD] Overwriting existing file: %s", filePath.c_str());
      esp_task_wdt_reset();
      Storage.remove(filePath.c_str());
    }

    // Open file for writing - this can be slow due to FAT cluster allocation
    esp_task_wdt_reset();
    if (!Storage.openFileForWrite("WEB", filePath, state.file)) {
      state.error = "Failed to create file on SD card";
      LOG_DBG("WEB", "[UPLOAD] FAILED to create file: %s", filePath.c_str());
      return;
    }
    esp_task_wdt_reset();

    LOG_DBG("WEB", "[UPLOAD] File created successfully: %s", filePath.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (state.file && state.error.isEmpty()) {
      // Buffer incoming data and flush when buffer is full
      // This reduces SD card write operations and improves throughput
      const uint8_t* data = upload.buf;
      size_t remaining = upload.currentSize;

      while (remaining > 0) {
        const size_t space = UploadState::UPLOAD_BUFFER_SIZE - state.bufferPos;
        const size_t toCopy = (remaining < space) ? remaining : space;

        memcpy(state.buffer.data() + state.bufferPos, data, toCopy);
        state.bufferPos += toCopy;
        data += toCopy;
        remaining -= toCopy;

        // Flush buffer when full
        if (state.bufferPos >= UploadState::UPLOAD_BUFFER_SIZE) {
          if (!flushUploadBuffer(state)) {
            state.error = "Failed to write to SD card - disk may be full";
            state.file.close();
            return;
          }
        }
      }

      state.size += upload.currentSize;

      // Log progress every 100KB
      if (state.size - lastLoggedSize >= 102400) {
        const unsigned long elapsed = millis() - uploadStartTime;
        const float kbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        LOG_DBG("WEB", "[UPLOAD] %d bytes (%.1f KB), %.1f KB/s, %d writes", state.size, state.size / 1024.0, kbps,
                writeCount);
        lastLoggedSize = state.size;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (state.file) {
      // Flush any remaining buffered data
      if (!flushUploadBuffer(state)) {
        state.error = "Failed to write final data to SD card";
      }
      state.file.close();

      if (state.error.isEmpty()) {
        state.success = true;
        const unsigned long elapsed = millis() - uploadStartTime;
        const float avgKbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        const float writePercent = (elapsed > 0) ? (totalWriteTime * 100.0 / elapsed) : 0;
        LOG_DBG("WEB", "[UPLOAD] Complete: %s (%d bytes in %lu ms, avg %.1f KB/s)", state.fileName.c_str(), state.size,
                elapsed, avgKbps);
        LOG_DBG("WEB", "[UPLOAD] Diagnostics: %d writes, total write time: %lu ms (%.1f%%)", writeCount, totalWriteTime,
                writePercent);

        // Clear epub cache to prevent stale metadata issues when overwriting files
        String filePath = state.path;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += state.fileName;
        clearEpubCacheIfNeeded(filePath);
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    state.bufferPos = 0;  // Discard buffered data
    if (state.file) {
      state.file.close();
      // Try to delete the incomplete file
      String filePath = state.path;
      if (!filePath.endsWith("/")) filePath += "/";
      filePath += state.fileName;
      Storage.remove(filePath.c_str());
    }
    state.error = "Upload aborted";
    LOG_DBG("WEB", "Upload aborted");
  }
}

void CrossPointWebServer::handleUploadPost(UploadState& state) const {
  if (state.success) {
    server->send(200, "text/plain", "File uploaded successfully: " + state.fileName);
  } else {
    const String error = state.error.isEmpty() ? "Unknown error during upload" : state.error;
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

  // Validate folder name
  if (folderName.isEmpty()) {
    server->send(400, "text/plain", "Folder name cannot be empty");
    return;
  }

  // Get parent path
  String parentPath = "/";
  if (server->hasArg("path")) {
    parentPath = server->arg("path");
    if (!parentPath.startsWith("/")) {
      parentPath = "/" + parentPath;
    }
    if (parentPath.length() > 1 && parentPath.endsWith("/")) {
      parentPath = parentPath.substring(0, parentPath.length() - 1);
    }
  }

  // Build full folder path
  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  LOG_DBG("WEB", "Creating folder: %s", folderPath.c_str());

  // Check if already exists
  if (Storage.exists(folderPath.c_str())) {
    server->send(400, "text/plain", "Folder already exists");
    return;
  }

  // Create the folder
  if (Storage.mkdir(folderPath.c_str())) {
    LOG_DBG("WEB", "Folder created successfully: %s", folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    LOG_DBG("WEB", "Failed to create folder: %s", folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void CrossPointWebServer::handleRename() const {
  if (!server->hasArg("path") || !server->hasArg("name")) {
    server->send(400, "text/plain", "Missing path or new name");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String newName = server->arg("name");
  newName.trim();

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (newName.isEmpty()) {
    server->send(400, "text/plain", "New name cannot be empty");
    return;
  }
  if (newName.indexOf('/') >= 0 || newName.indexOf('\\') >= 0) {
    server->send(400, "text/plain", "Invalid file name");
    return;
  }
  if (isProtectedItemName(newName)) {
    server->send(403, "text/plain", "Cannot rename to protected name");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (isProtectedItemName(itemName)) {
    server->send(403, "text/plain", "Cannot rename protected item");
    return;
  }
  if (newName == itemName) {
    server->send(200, "text/plain", "Name unchanged");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be renamed");
    return;
  }

  String parentPath = itemPath.substring(0, itemPath.lastIndexOf('/'));
  if (parentPath.isEmpty()) {
    parentPath = "/";
  }
  String newPath = parentPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += newName;

  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  clearEpubCacheIfNeeded(itemPath);
  const bool success = file.rename(newPath.c_str());
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
  if (!server->hasArg("path") || !server->hasArg("dest")) {
    server->send(400, "text/plain", "Missing path or destination");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String destPath = normalizeWebPath(server->arg("dest"));

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (destPath.isEmpty()) {
    server->send(400, "text/plain", "Invalid destination");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (isProtectedItemName(itemName)) {
    server->send(403, "text/plain", "Cannot move protected item");
    return;
  }
  if (destPath != "/") {
    const String destName = destPath.substring(destPath.lastIndexOf('/') + 1);
    if (isProtectedItemName(destName)) {
      server->send(403, "text/plain", "Cannot move into protected folder");
      return;
    }
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be moved");
    return;
  }

  if (!Storage.exists(destPath.c_str())) {
    file.close();
    server->send(404, "text/plain", "Destination not found");
    return;
  }
  FsFile destDir = Storage.open(destPath.c_str());
  if (!destDir || !destDir.isDirectory()) {
    if (destDir) {
      destDir.close();
    }
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
  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  clearEpubCacheIfNeeded(itemPath);
  const bool success = file.rename(newPath.c_str());
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
  // Check if 'paths' argument is provided
  if (!server->hasArg("paths")) {
    server->send(400, "text/plain", "Missing paths");
    return;
  }

  // Parse paths
  String pathsArg = server->arg("paths");
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, pathsArg);
  if (error) {
    server->send(400, "text/plain", "Invalid paths format");
    return;
  }

  auto paths = doc.as<JsonArray>();
  if (paths.isNull() || paths.size() == 0) {
    server->send(400, "text/plain", "No paths provided");
    return;
  }

  // Iterate over paths and delete each item
  bool allSuccess = true;
  String failedItems;

  for (const auto& p : paths) {
    auto itemPath = p.as<String>();

    // Validate path
    if (itemPath.isEmpty() || itemPath == "/") {
      failedItems += itemPath + " (cannot delete root); ";
      allSuccess = false;
      continue;
    }

    // Ensure path starts with /
    if (!itemPath.startsWith("/")) {
      itemPath = "/" + itemPath;
    }

    // Security check: prevent deletion of protected items
    const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

    // Hidden/system files are protected
    if (itemName.startsWith(".")) {
      failedItems += itemPath + " (hidden/system file); ";
      allSuccess = false;
      continue;
    }

    // Check against explicitly protected items
    bool isProtected = false;
    for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
      if (itemName.equals(HIDDEN_ITEMS[i])) {
        isProtected = true;
        break;
      }
    }
    if (isProtected) {
      failedItems += itemPath + " (protected file); ";
      allSuccess = false;
      continue;
    }

    // Check if item exists
    if (!Storage.exists(itemPath.c_str())) {
      failedItems += itemPath + " (not found); ";
      allSuccess = false;
      continue;
    }

    // Decide whether it's a directory or file by opening it
    bool success = false;
    FsFile f = Storage.open(itemPath.c_str());
    if (f && f.isDirectory()) {
      // For folders, ensure empty before removing
      FsFile entry = f.openNextFile();
      if (entry) {
        entry.close();
        f.close();
        failedItems += itemPath + " (folder not empty); ";
        allSuccess = false;
        continue;
      }
      f.close();
      success = Storage.rmdir(itemPath.c_str());
    } else {
      // It's a file (or couldn't open as dir) — remove file
      if (f) f.close();
      success = Storage.remove(itemPath.c_str());
      clearEpubCacheIfNeeded(itemPath);
    }

    if (!success) {
      failedItems += itemPath + " (deletion failed); ";
      allSuccess = false;
    }
  }

  if (allSuccess) {
    server->send(200, "text/plain", "All items deleted successfully");
  } else {
    server->send(500, "text/plain", "Failed to delete some items: " + failedItems);
  }
}

void CrossPointWebServer::handleSettingsPage() const {
  sendHtmlContent(server.get(), SettingsPageHtml, sizeof(SettingsPageHtml));
  LOG_DBG("WEB", "Served settings page");
}

void CrossPointWebServer::handleGetSettings() const {
  const auto& settings = getSettingsList();

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  static char output[2048];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;
  bool fontsScanned = false;

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
        JsonArray options = doc["options"].to<JsonArray>();

        if (isReaderFontFamilySetting(s)) {
          if (!fontsScanned) {
            FontMgr.scanFonts();
            fontsScanned = true;
          }

          const int builtinCount = static_cast<int>(s.enumValues.size());
          const int selectedExternal = FontMgr.getSelectedIndex();
          if (selectedExternal >= 0) {
            doc["value"] = builtinCount + selectedExternal;
          } else if (s.valuePtr) {
            doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
          } else {
            doc["value"] = 0;
          }

          for (const auto& opt : s.enumValues) {
            options.add(I18N.get(opt));
          }

          for (int i = 0; i < FontMgr.getFontCount(); i++) {
            const FontInfo* info = FontMgr.getFontInfo(i);
            if (!info) continue;
            std::string label = std::string(info->name) + " (" + std::to_string(info->size) + "pt)";
            if (!ExternalFont::canFitGlyph(info->width, info->height)) {
              label += " [!]";
            }
            options.add(label);
          }
        } else {
          if (s.valuePtr) {
            doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
          } else if (s.valueGetter) {
            doc["value"] = static_cast<int>(s.valueGetter());
          }
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
        if (s.stringGetter) {
          doc["value"] = s.stringGetter();
        } else if (s.stringPtr) {
          doc["value"] = s.stringPtr;
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

  // Add UI font selector for web settings.
  // Device UI already has a dedicated action page for this; web gets an enum list.
  doc.clear();
  doc["key"] = "uiFontFamily";
  doc["name"] = I18N.get(StrId::STR_EXT_UI_FONT);
  doc["category"] = I18N.get(StrId::STR_CAT_DISPLAY);
  doc["type"] = "enum";
  if (!fontsScanned) {
    FontMgr.scanFonts();
    fontsScanned = true;
  }
  const int selectedUiExternal = FontMgr.getUiSelectedIndex();
  doc["value"] = selectedUiExternal >= 0 ? selectedUiExternal + 1 : 0;

  JsonArray uiOptions = doc["options"].to<JsonArray>();
  uiOptions.add(I18N.get(StrId::STR_BUILTIN_DISABLED));
  for (int i = 0; i < FontMgr.getFontCount(); i++) {
    const FontInfo* info = FontMgr.getFontInfo(i);
    if (!info) continue;
    std::string label = std::string(info->name) + " (" + std::to_string(info->size) + "pt)";
    if (!ExternalFont::canFitGlyph(info->width, info->height)) {
      label += " [!]";
    }
    uiOptions.add(label);
  }

  const size_t uiWritten = serializeJson(doc, output, outputSize);
  if (uiWritten < outputSize) {
    if (seenFirst) {
      server->sendContent(",");
    }
    seenFirst = true;
    server->sendContent(output);
  } else {
    LOG_DBG("WEB", "Skipping oversized setting JSON for: uiFontFamily");
  }

  // Add language selector for web settings.
  doc.clear();
  doc["key"] = "language";
  doc["name"] = I18N.get(StrId::STR_LANGUAGE);
  doc["category"] = I18N.get(StrId::STR_CAT_SYSTEM);
  doc["type"] = "enum";
  doc["value"] = static_cast<int>(I18N.getLanguage());

  JsonArray languageOptions = doc["options"].to<JsonArray>();
  for (int i = 0; i < static_cast<int>(getLanguageCount()); i++) {
    languageOptions.add(I18N.getLanguageName(static_cast<Language>(i)));
  }

  const size_t langWritten = serializeJson(doc, output, outputSize);
  if (langWritten < outputSize) {
    if (seenFirst) {
      server->sendContent(",");
    }
    server->sendContent(output);
  } else {
    LOG_DBG("WEB", "Skipping oversized setting JSON for: language");
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
  bool fontsScanned = false;

  if (doc["uiFontFamily"].is<JsonVariant>()) {
    const int val = doc["uiFontFamily"].as<int>();
    if (!fontsScanned) {
      FontMgr.scanFonts();
      fontsScanned = true;
    }

    if (val == 0) {
      FontMgr.selectUiFont(-1);
      applied++;
    } else {
      const int externalIndex = val - 1;
      const FontInfo* info = FontMgr.getFontInfo(externalIndex);
      if (info && ExternalFont::canFitGlyph(info->width, info->height)) {
        FontMgr.selectUiFont(externalIndex);
        applied++;
      }
    }
  }

  if (doc["language"].is<JsonVariant>()) {
    const int val = doc["language"].as<int>();
    if (val >= 0 && val < static_cast<int>(getLanguageCount())) {
      I18N.setLanguage(static_cast<Language>(val));
      applied++;
    }
  }

  for (const auto& s : settings) {
    if (isUiFontFamilyKey(s.key) || isLanguageSettingKey(s.key)) continue;
    if (!s.key) continue;
    if (!doc[s.key].is<JsonVariant>()) continue;

    switch (s.type) {
      case SettingType::TOGGLE: {
        const int val = doc[s.key].as<int>() ? 1 : 0;
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = val;
        }
        applied++;
        break;
      }
      case SettingType::ENUM: {
        const int val = doc[s.key].as<int>();
        if (isReaderFontFamilySetting(s)) {
          if (!fontsScanned) {
            FontMgr.scanFonts();
            fontsScanned = true;
          }

          const int builtinCount = static_cast<int>(s.enumValues.size());
          if (val >= 0 && val < builtinCount) {
            if (s.valuePtr) {
              SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
            }
            // Switch to built-in family selected in web UI.
            FontMgr.selectFont(-1);
            applied++;
          } else {
            const int externalIndex = val - builtinCount;
            const FontInfo* info = FontMgr.getFontInfo(externalIndex);
            if (info && ExternalFont::canFitGlyph(info->width, info->height)) {
              FontMgr.selectFont(externalIndex);
              applied++;
            }
          }
          break;
        }

        if (val >= 0 && val < static_cast<int>(s.enumValues.size())) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          } else if (s.valueSetter) {
            s.valueSetter(static_cast<uint8_t>(val));
          }
          applied++;
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
        }
        break;
      }
      case SettingType::STRING: {
        const std::string val = doc[s.key].as<std::string>();
        if (s.stringSetter) {
          s.stringSetter(val);
        } else if (s.stringPtr && s.stringMaxLen > 0) {
          strncpy(s.stringPtr, val.c_str(), s.stringMaxLen - 1);
          s.stringPtr[s.stringMaxLen - 1] = '\0';
        }
        applied++;
        break;
      }
      default:
        break;
    }
  }

  SETTINGS.saveToFile();

  LOG_DBG("WEB", "Applied %d setting(s)", applied);
  server->send(200, "text/plain", String("Applied ") + String(applied) + " setting(s)");
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
  switch (type) {
    case WStype_DISCONNECTED:
      LOG_DBG("WS", "Client %u disconnected", num);
      // Clean up any in-progress upload
      if (wsUploadInProgress && wsUploadFile) {
        wsUploadFile.close();
        // Delete incomplete file
        String filePath = wsUploadPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += wsUploadFileName;
        Storage.remove(filePath.c_str());
        LOG_DBG("WS", "Deleted incomplete upload: %s", filePath.c_str());
      }
      wsUploadInProgress = false;
      break;

    case WStype_CONNECTED: {
      LOG_DBG("WS", "Client %u connected", num);
      break;
    }

    case WStype_TEXT: {
      // Parse control messages
      String msg = String((char*)payload);
      LOG_DBG("WS", "Text from client %u: %s", num, msg.c_str());

      if (msg.startsWith("START:")) {
        // Parse: START:<filename>:<size>:<path>
        int firstColon = msg.indexOf(':', 6);
        int secondColon = msg.indexOf(':', firstColon + 1);

        if (firstColon > 0 && secondColon > 0) {
          wsUploadFileName = msg.substring(6, firstColon);
          wsUploadSize = msg.substring(firstColon + 1, secondColon).toInt();
          wsUploadPath = msg.substring(secondColon + 1);
          wsUploadReceived = 0;
          wsUploadStartTime = millis();

          // Ensure path is valid
          if (!wsUploadPath.startsWith("/")) wsUploadPath = "/" + wsUploadPath;
          if (wsUploadPath.length() > 1 && wsUploadPath.endsWith("/")) {
            wsUploadPath = wsUploadPath.substring(0, wsUploadPath.length() - 1);
          }

          // Build file path
          String filePath = wsUploadPath;
          if (!filePath.endsWith("/")) filePath += "/";
          filePath += wsUploadFileName;

          LOG_DBG("WS", "Starting upload: %s (%d bytes) to %s", wsUploadFileName.c_str(), wsUploadSize,
                  filePath.c_str());

          // Check if file exists and remove it
          esp_task_wdt_reset();
          if (Storage.exists(filePath.c_str())) {
            Storage.remove(filePath.c_str());
          }

          // Open file for writing
          esp_task_wdt_reset();
          if (!Storage.openFileForWrite("WS", filePath, wsUploadFile)) {
            wsServer->sendTXT(num, "ERROR:Failed to create file");
            wsUploadInProgress = false;
            return;
          }
          esp_task_wdt_reset();

          wsUploadInProgress = true;
          wsServer->sendTXT(num, "READY");
        } else {
          wsServer->sendTXT(num, "ERROR:Invalid START format");
        }
      }
      break;
    }

    case WStype_BIN: {
      if (!wsUploadInProgress || !wsUploadFile) {
        wsServer->sendTXT(num, "ERROR:No upload in progress");
        return;
      }

      // Write binary data directly to file
      esp_task_wdt_reset();
      size_t written = wsUploadFile.write(payload, length);
      esp_task_wdt_reset();

      if (written != length) {
        wsUploadFile.close();
        wsUploadInProgress = false;
        wsServer->sendTXT(num, "ERROR:Write failed - disk full?");
        return;
      }

      wsUploadReceived += written;

      // Send progress update (every 64KB or at end)
      static size_t lastProgressSent = 0;
      if (wsUploadReceived - lastProgressSent >= 65536 || wsUploadReceived >= wsUploadSize) {
        String progress = "PROGRESS:" + String(wsUploadReceived) + ":" + String(wsUploadSize);
        wsServer->sendTXT(num, progress);
        lastProgressSent = wsUploadReceived;
      }

      // Check if upload complete
      if (wsUploadReceived >= wsUploadSize) {
        wsUploadFile.close();
        wsUploadInProgress = false;

        wsLastCompleteName = wsUploadFileName;
        wsLastCompleteSize = wsUploadSize;
        wsLastCompleteAt = millis();

        unsigned long elapsed = millis() - wsUploadStartTime;
        float kbps = (elapsed > 0) ? (wsUploadSize / 1024.0) / (elapsed / 1000.0) : 0;

        LOG_DBG("WS", "Upload complete: %s (%d bytes in %lu ms, %.1f KB/s)", wsUploadFileName.c_str(), wsUploadSize,
                elapsed, kbps);

        // Clear epub cache to prevent stale metadata issues when overwriting files
        String filePath = wsUploadPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += wsUploadFileName;
        clearEpubCacheIfNeeded(filePath);

        wsServer->sendTXT(num, "DONE");
        lastProgressSent = 0;
      }
      break;
    }

    default:
      break;
  }
}

// --- WiFi credential management API handlers (CJK) ---

void CrossPointWebServer::handleWifiScan() const {
  LOG_DBG("WEB", "WiFi scan requested");

  // In AP mode we need to briefly enable STA to scan, without tearing down the AP.
  const wifi_mode_t prevMode = WiFi.getMode();
  if (apMode) {
    WiFi.mode(WIFI_AP_STA);
    delay(100);
  }

  // Use async scan to avoid long blocking calls that can trigger task watchdog resets.
  const unsigned long scanStart = millis();
  constexpr unsigned long SCAN_TIMEOUT_MS = 20000;
  WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);

  int n = WIFI_SCAN_RUNNING;
  while (n == WIFI_SCAN_RUNNING && (millis() - scanStart) < SCAN_TIMEOUT_MS) {
    esp_task_wdt_reset();
    delay(20);
    n = WiFi.scanComplete();
  }

  // Restore previous WiFi mode after scan
  if (apMode && prevMode != WIFI_AP_STA) {
    WiFi.mode(prevMode);
  }

  if (n == WIFI_SCAN_RUNNING) {
    WiFi.scanDelete();
    server->send(500, "application/json", "{\"error\":\"Scan timeout\"}");
    LOG_ERR("WEB", "WiFi scan timed out after %lu ms", millis() - scanStart);
    return;
  }

  if (n < 0) {
    server->send(500, "application/json", "{\"error\":\"Scan failed\"}");
    LOG_ERR("WEB", "WiFi scan failed with code %d", n);
    return;
  }

  // De-duplicate by SSID, keeping the strongest signal
  std::map<String, int> bestIndex;
  for (int i = 0; i < n; i++) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;  // Skip hidden networks
    auto it = bestIndex.find(ssid);
    if (it == bestIndex.end() || WiFi.RSSI(i) > WiFi.RSSI(it->second)) {
      bestIndex[ssid] = i;
    }
  }

  // Build JSON array
  String json = "[";
  bool first = true;
  for (const auto& entry : bestIndex) {
    const int i = entry.second;
    if (!first) json += ",";
    first = false;

    JsonDocument doc;
    doc["ssid"] = WiFi.SSID(i);
    doc["rssi"] = WiFi.RSSI(i);
    doc["encrypted"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    doc["saved"] = WIFI_STORE.hasSavedCredential(WiFi.SSID(i).c_str());

    char buf[256];
    serializeJson(doc, buf, sizeof(buf));
    json += buf;
  }
  json += "]";

  WiFi.scanDelete();
  server->send(200, "application/json", json);
  LOG_DBG("WEB", "WiFi scan returned %d unique networks", bestIndex.size());
}

void CrossPointWebServer::handleWifiSave() const {
  // Expect JSON body: {"ssid": "...", "password": "..."}
  if (!server->hasArg("plain")) {
    server->send(400, "application/json", "{\"error\":\"Missing request body\"}");
    return;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, server->arg("plain"));
  if (err) {
    server->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  const char* ssid = doc["ssid"];
  const char* password = doc["password"];

  if (!ssid || strlen(ssid) == 0) {
    server->send(400, "application/json", "{\"error\":\"SSID is required\"}");
    return;
  }
  if (!password) {
    server->send(400, "application/json", "{\"error\":\"Password is required\"}");
    return;
  }

  // Load existing credentials, add/update, then save
  WIFI_STORE.loadFromFile();
  const bool ok = WIFI_STORE.addCredential(ssid, password);

  if (ok) {
    server->send(200, "application/json", "{\"success\":true}");
    LOG_DBG("WEB", "WiFi credential saved for SSID: %s", ssid);
  } else {
    server->send(500, "application/json", "{\"error\":\"Failed to save credential\"}");
    LOG_ERR("WEB", "Failed to save WiFi credential for SSID: %s", ssid);
  }
}

void CrossPointWebServer::handleWifiList() const {
  WIFI_STORE.loadFromFile();
  const auto& creds = WIFI_STORE.getCredentials();

  String json = "[";
  for (size_t i = 0; i < creds.size(); i++) {
    if (i > 0) json += ",";
    // Only expose SSID, never the password
    json += "{\"ssid\":\"";
    // Escape any quotes in SSID
    String escaped = creds[i].ssid.c_str();
    escaped.replace("\"", "\\\"");
    json += escaped;
    json += "\"}";
  }
  json += "]";

  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleWifiDelete() const {
  if (!server->hasArg("plain")) {
    server->send(400, "application/json", "{\"error\":\"Missing request body\"}");
    return;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, server->arg("plain"));
  if (err) {
    server->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  const char* ssid = doc["ssid"];
  if (!ssid || strlen(ssid) == 0) {
    server->send(400, "application/json", "{\"error\":\"SSID is required\"}");
    return;
  }

  WIFI_STORE.loadFromFile();
  const bool ok = WIFI_STORE.removeCredential(ssid);

  if (ok) {
    server->send(200, "application/json", "{\"success\":true}");
    LOG_DBG("WEB", "WiFi credential deleted for SSID: %s", ssid);
  } else {
    server->send(404, "application/json", "{\"error\":\"Credential not found\"}");
  }
}
