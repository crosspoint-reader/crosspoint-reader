#include "CrossPointWebServer.h"

#include <SD.h>
#include <WiFi.h>

#include <algorithm>

#include "config.h"
#include "html/FilesPageFooterHtml.generated.h"
#include "html/FilesPageHeaderHtml.generated.h"
#include "html/HomePageHtml.generated.h"

namespace {

// Folders/files to hide from the web interface file browser
// Note: Items starting with "." are automatically hidden
const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
const size_t HIDDEN_ITEMS_COUNT = sizeof(HIDDEN_ITEMS) / sizeof(HIDDEN_ITEMS[0]);

// Helper function to escape HTML special characters to prevent XSS
String escapeHtml(const String& input) {
  String output;
  output.reserve(input.length() * 1.1);  // Pre-allocate with some extra space

  for (size_t i = 0; i < input.length(); i++) {
    char c = input.charAt(i);
    switch (c) {
      case '&':
        output += "&amp;";
        break;
      case '<':
        output += "&lt;";
        break;
      case '>':
        output += "&gt;";
        break;
      case '"':
        output += "&quot;";
        break;
      case '\'':
        output += "&#39;";
        break;
      default:
        output += c;
        break;
    }
  }
  return output;
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
    Serial.printf("[%lu] [WEB] Web server already running\n", millis());
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[%lu] [WEB] Cannot start webserver - WiFi not connected\n", millis());
    return;
  }

  Serial.printf("[%lu] [WEB] [MEM] Free heap before begin: %d bytes\n", millis(), ESP.getFreeHeap());

  Serial.printf("[%lu] [WEB] Creating web server on port %d...\n", millis(), port);
  server = new WebServer(port);
  Serial.printf("[%lu] [WEB] [MEM] Free heap after WebServer allocation: %d bytes\n", millis(), ESP.getFreeHeap());

  if (!server) {
    Serial.printf("[%lu] [WEB] Failed to create WebServer!\n", millis());
    return;
  }

  // Setup routes
  Serial.printf("[%lu] [WEB] Setting up routes...\n", millis());
  server->on("/", HTTP_GET, [this]() { handleRoot(); });
  server->on("/status", HTTP_GET, [this]() { handleStatus(); });
  server->on("/files", HTTP_GET, [this]() { handleFileList(); });

  // Upload endpoint with special handling for multipart form data
  server->on("/upload", HTTP_POST, [this]() { handleUploadPost(); }, [this]() { handleUpload(); });

  // Create folder endpoint
  server->on("/mkdir", HTTP_POST, [this]() { handleCreateFolder(); });

  // Delete file/folder endpoint
  server->on("/delete", HTTP_POST, [this]() { handleDelete(); });

  server->onNotFound([this]() { handleNotFound(); });
  Serial.printf("[%lu] [WEB] [MEM] Free heap after route setup: %d bytes\n", millis(), ESP.getFreeHeap());

  server->begin();
  running = true;

  Serial.printf("[%lu] [WEB] Web server started on port %d\n", millis(), port);
  Serial.printf("[%lu] [WEB] Access at http://%s/\n", millis(), WiFi.localIP().toString().c_str());
  Serial.printf("[%lu] [WEB] [MEM] Free heap after server.begin(): %d bytes\n", millis(), ESP.getFreeHeap());
}

void CrossPointWebServer::stop() {
  if (!running || !server) {
    Serial.printf("[%lu] [WEB] stop() called but already stopped (running=%d, server=%p)\n", millis(), running, server);
    return;
  }

  Serial.printf("[%lu] [WEB] STOP INITIATED - setting running=false first\n", millis());
  running = false;  // Set this FIRST to prevent handleClient from using server

  Serial.printf("[%lu] [WEB] [MEM] Free heap before stop: %d bytes\n", millis(), ESP.getFreeHeap());

  // Add delay to allow any in-flight handleClient() calls to complete
  delay(100);
  Serial.printf("[%lu] [WEB] Waited 100ms for handleClient to finish\n", millis());

  server->stop();
  Serial.printf("[%lu] [WEB] [MEM] Free heap after server->stop(): %d bytes\n", millis(), ESP.getFreeHeap());

  // Add another delay before deletion to ensure server->stop() completes
  delay(50);
  Serial.printf("[%lu] [WEB] Waited 50ms before deleting server\n", millis());

  delete server;
  server = nullptr;

  Serial.printf("[%lu] [WEB] Web server stopped and deleted\n", millis());
  Serial.printf("[%lu] [WEB] [MEM] Free heap after delete server: %d bytes\n", millis(), ESP.getFreeHeap());

  // Note: Static upload variables (uploadFileName, uploadPath, uploadError) are declared
  // later in the file and will be cleared when they go out of scope or on next upload
  Serial.printf("[%lu] [WEB] [MEM] Free heap final: %d bytes\n", millis(), ESP.getFreeHeap());
}

void CrossPointWebServer::handleClient() {
  static unsigned long lastDebugPrint = 0;

  // Check running flag FIRST before accessing server
  if (!running) {
    return;
  }

  // Double-check server pointer is valid
  if (!server) {
    Serial.printf("[%lu] [WEB] WARNING: handleClient called with null server!\n", millis());
    return;
  }

  // Print debug every 10 seconds to confirm handleClient is being called
  if (millis() - lastDebugPrint > 10000) {
    Serial.printf("[%lu] [WEB] handleClient active, server running on port %d\n", millis(), port);
    lastDebugPrint = millis();
  }

  server->handleClient();
}

void CrossPointWebServer::handleRoot() {
  String html = HomePageHtml;

  // Replace placeholders with actual values
  html.replace("%VERSION%", CROSSPOINT_VERSION);
  html.replace("%IP_ADDRESS%", WiFi.localIP().toString());
  html.replace("%FREE_HEAP%", String(ESP.getFreeHeap()));

  server->send(200, "text/html", html);
  Serial.printf("[%lu] [WEB] Served root page\n", millis());
}

void CrossPointWebServer::handleNotFound() {
  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

void CrossPointWebServer::handleStatus() {
  String json = "{";
  json += "\"version\":\"" + String(CROSSPOINT_VERSION) + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"uptime\":" + String(millis() / 1000);
  json += "}";

  server->send(200, "application/json", json);
}

std::vector<FileInfo> CrossPointWebServer::scanFiles(const char* path) {
  std::vector<FileInfo> files;

  File root = SD.open(path);
  if (!root) {
    Serial.printf("[%lu] [WEB] Failed to open directory: %s\n", millis(), path);
    return files;
  }

  if (!root.isDirectory()) {
    Serial.printf("[%lu] [WEB] Not a directory: %s\n", millis(), path);
    root.close();
    return files;
  }

  Serial.printf("[%lu] [WEB] Scanning files in: %s\n", millis(), path);

  File file = root.openNextFile();
  while (file) {
    String fileName = String(file.name());

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

      files.push_back(info);
    }

    file.close();
    file = root.openNextFile();
  }
  root.close();

  Serial.printf("[%lu] [WEB] Found %d items (files and folders)\n", millis(), files.size());
  return files;
}

String CrossPointWebServer::formatFileSize(size_t bytes) {
  if (bytes < 1024) {
    return String(bytes) + " B";
  } else if (bytes < 1024 * 1024) {
    return String(bytes / 1024.0, 1) + " KB";
  } else {
    return String(bytes / (1024.0 * 1024.0), 1) + " MB";
  }
}

bool CrossPointWebServer::isEpubFile(const String& filename) {
  String lower = filename;
  lower.toLowerCase();
  return lower.endsWith(".epub");
}

void CrossPointWebServer::handleFileList() {
  String html = FilesPageHeaderHtml;

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

  // Get message from query string if present
  if (server->hasArg("msg")) {
    String msg = escapeHtml(server->arg("msg"));
    String msgType = server->hasArg("type") ? escapeHtml(server->arg("type")) : "success";
    html += "<div class=\"message " + msgType + "\">" + msg + "</div>";
  }

  // Hidden input to store current path for JavaScript
  html += "<input type=\"hidden\" id=\"currentPath\" value=\"" + currentPath + "\">";

  // Scan files in current path first (we need counts for the header)
  std::vector<FileInfo> files = scanFiles(currentPath.c_str());

  // Count items
  int epubCount = 0;
  int folderCount = 0;
  size_t totalSize = 0;
  for (const auto& file : files) {
    if (file.isDirectory) {
      folderCount++;
    } else {
      if (file.isEpub) epubCount++;
      totalSize += file.size;
    }
  }

  // Page header with inline breadcrumb and action buttons
  html += "<div class=\"page-header\">";
  html += "<div class=\"page-header-left\">";
  html += "<h1>📁 File Manager</h1>";

  // Inline breadcrumb
  html += "<div class=\"breadcrumb-inline\">";
  html += "<span class=\"sep\">/</span>";

  if (currentPath == "/") {
    html += "<span class=\"current\">🏠</span>";
  } else {
    html += "<a href=\"/files\">🏠</a>";
    String pathParts = currentPath.substring(1);  // Remove leading /
    String buildPath = "";
    int start = 0;
    int end = pathParts.indexOf('/');

    while (start < (int)pathParts.length()) {
      String part;
      if (end == -1) {
        part = pathParts.substring(start);
        buildPath += "/" + part;
        html += "<span class=\"sep\">/</span><span class=\"current\">" + escapeHtml(part) + "</span>";
        break;
      } else {
        part = pathParts.substring(start, end);
        buildPath += "/" + part;
        html += "<span class=\"sep\">/</span><a href=\"/files?path=" + buildPath + "\">" + escapeHtml(part) + "</a>";
        start = end + 1;
        end = pathParts.indexOf('/', start);
      }
    }
  }
  html += "</div>";
  html += "</div>";

  // Action buttons
  html += "<div class=\"action-buttons\">";
  html += "<button class=\"action-btn upload-action-btn\" onclick=\"openUploadModal()\">";
  html += "📤 Upload";
  html += "</button>";
  html += "<button class=\"action-btn folder-action-btn\" onclick=\"openFolderModal()\">";
  html += "📁 New Folder";
  html += "</button>";
  html += "</div>";

  html += "</div>";  // end page-header

  // Contents card with inline summary
  html += "<div class=\"card\">";

  // Contents header with inline stats
  html += "<div class=\"contents-header\">";
  html += "<h2 class=\"contents-title\">Contents</h2>";
  html += "<span class=\"summary-inline\">";
  html += String(folderCount) + " folder" + (folderCount != 1 ? "s" : "") + ", ";
  html += String(files.size() - folderCount) + " file" + ((files.size() - folderCount) != 1 ? "s" : "") + ", ";
  html += formatFileSize(totalSize);
  html += "</span>";
  html += "</div>";

  if (files.empty()) {
    html += "<div class=\"no-files\">This folder is empty</div>";
  } else {
    html += "<table class=\"file-table\">";
    html += "<tr><th>Name</th><th>Type</th><th>Size</th><th class=\"actions-col\">Actions</th></tr>";

    // Sort files: folders first, then epub files, then other files, alphabetically within each group
    std::sort(files.begin(), files.end(), [](const FileInfo& a, const FileInfo& b) {
      // Folders come first
      if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
      // Then sort by epub status (epubs first among files)
      if (!a.isDirectory && !b.isDirectory) {
        if (a.isEpub != b.isEpub) return a.isEpub > b.isEpub;
      }
      // Then alphabetically
      return a.name < b.name;
    });

    for (const auto& file : files) {
      String rowClass;
      String icon;
      String badge;
      String typeStr;
      String sizeStr;

      if (file.isDirectory) {
        rowClass = "folder-row";
        icon = "📁";
        badge = "<span class=\"folder-badge\">FOLDER</span>";
        typeStr = "Folder";
        sizeStr = "-";

        // Build the path to this folder
        String folderPath = currentPath;
        if (!folderPath.endsWith("/")) folderPath += "/";
        folderPath += file.name;

        html += "<tr class=\"" + rowClass + "\">";
        html += "<td><span class=\"file-icon\">" + icon + "</span>";
        html += "<a href=\"/files?path=" + folderPath + "\" class=\"folder-link\">" + escapeHtml(file.name) + "</a>" +
                badge + "</td>";
        html += "<td>" + typeStr + "</td>";
        html += "<td>" + sizeStr + "</td>";
        // Escape quotes for JavaScript string
        String escapedName = file.name;
        escapedName.replace("'", "\\'");
        String escapedPath = folderPath;
        escapedPath.replace("'", "\\'");
        html += "<td class=\"actions-col\"><button class=\"delete-btn\" onclick=\"openDeleteModal('" + escapedName +
                "', '" + escapedPath + "', true)\" title=\"Delete folder\">🗑️</button></td>";
        html += "</tr>";
      } else {
        rowClass = file.isEpub ? "epub-file" : "";
        icon = file.isEpub ? "📗" : "📄";
        badge = file.isEpub ? "<span class=\"epub-badge\">EPUB</span>" : "";
        String ext = file.name.substring(file.name.lastIndexOf('.') + 1);
        ext.toUpperCase();
        typeStr = ext;
        sizeStr = formatFileSize(file.size);

        // Build file path for delete
        String filePath = currentPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += file.name;

        html += "<tr class=\"" + rowClass + "\">";
        html += "<td><span class=\"file-icon\">" + icon + "</span>" + escapeHtml(file.name) + badge + "</td>";
        html += "<td>" + typeStr + "</td>";
        html += "<td>" + sizeStr + "</td>";
        // Escape quotes for JavaScript string
        String escapedName = file.name;
        escapedName.replace("'", "\\'");
        String escapedPath = filePath;
        escapedPath.replace("'", "\\'");
        html += "<td class=\"actions-col\"><button class=\"delete-btn\" onclick=\"openDeleteModal('" + escapedName +
                "', '" + escapedPath + "', false)\" title=\"Delete file\">🗑️</button></td>";
        html += "</tr>";
      }
    }

    html += "</table>";
  }

  html += "</div>";

  html += FilesPageFooterHtml;

  server->send(200, "text/html", html);
  Serial.printf("[%lu] [WEB] Served file listing page for path: %s\n", millis(), currentPath.c_str());
}

// Static variables for upload handling
static File uploadFile;
static String uploadFileName;
static String uploadPath = "/";
static size_t uploadSize = 0;
static bool uploadSuccess = false;
static String uploadError = "";

void CrossPointWebServer::handleUpload() {
  static unsigned long lastWriteTime = 0;
  static unsigned long uploadStartTime = 0;
  static size_t lastLoggedSize = 0;

  // Safety check: ensure server is still valid
  if (!running || !server) {
    Serial.printf("[%lu] [WEB] [UPLOAD] ERROR: handleUpload called but server not running!\n", millis());
    return;
  }

  HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    uploadFileName = upload.filename;
    uploadSize = 0;
    uploadSuccess = false;
    uploadError = "";
    uploadStartTime = millis();
    lastWriteTime = millis();
    lastLoggedSize = 0;

    // Get upload path from query parameter (defaults to root if not specified)
    // Note: We use query parameter instead of form data because multipart form
    // fields aren't available until after file upload completes
    if (server->hasArg("path")) {
      uploadPath = server->arg("path");
      // Ensure path starts with /
      if (!uploadPath.startsWith("/")) {
        uploadPath = "/" + uploadPath;
      }
      // Remove trailing slash unless it's root
      if (uploadPath.length() > 1 && uploadPath.endsWith("/")) {
        uploadPath = uploadPath.substring(0, uploadPath.length() - 1);
      }
    } else {
      uploadPath = "/";
    }

    Serial.printf("[%lu] [WEB] [UPLOAD] START: %s to path: %s\n", millis(), uploadFileName.c_str(), uploadPath.c_str());
    Serial.printf("[%lu] [WEB] [UPLOAD] Free heap: %d bytes\n", millis(), ESP.getFreeHeap());

    // Create file path
    String filePath = uploadPath;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += uploadFileName;

    // Check if file already exists
    if (SD.exists(filePath.c_str())) {
      Serial.printf("[%lu] [WEB] [UPLOAD] Overwriting existing file: %s\n", millis(), filePath.c_str());
      SD.remove(filePath.c_str());
    }

    // Open file for writing
    uploadFile = SD.open(filePath.c_str(), FILE_WRITE);
    if (!uploadFile) {
      uploadError = "Failed to create file on SD card";
      Serial.printf("[%lu] [WEB] [UPLOAD] FAILED to create file: %s\n", millis(), filePath.c_str());
      return;
    }

    Serial.printf("[%lu] [WEB] [UPLOAD] File created successfully: %s\n", millis(), filePath.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile && uploadError.isEmpty()) {
      unsigned long writeStartTime = millis();
      size_t written = uploadFile.write(upload.buf, upload.currentSize);
      unsigned long writeEndTime = millis();
      unsigned long writeDuration = writeEndTime - writeStartTime;

      if (written != upload.currentSize) {
        uploadError = "Failed to write to SD card - disk may be full";
        uploadFile.close();
        Serial.printf("[%lu] [WEB] [UPLOAD] WRITE ERROR - expected %d, wrote %d\n", millis(), upload.currentSize,
                      written);
      } else {
        uploadSize += written;

        // Log progress every 50KB or if write took >100ms
        if (uploadSize - lastLoggedSize >= 51200 || writeDuration > 100) {
          unsigned long timeSinceStart = millis() - uploadStartTime;
          unsigned long timeSinceLastWrite = millis() - lastWriteTime;
          float kbps = (uploadSize / 1024.0) / (timeSinceStart / 1000.0);

          Serial.printf(
              "[%lu] [WEB] [UPLOAD] Progress: %d bytes (%.1f KB), %.1f KB/s, write took %lu ms, gap since last: %lu "
              "ms\n",
              millis(), uploadSize, uploadSize / 1024.0, kbps, writeDuration, timeSinceLastWrite);
          lastLoggedSize = uploadSize;
        }
        lastWriteTime = millis();
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();

      if (uploadError.isEmpty()) {
        uploadSuccess = true;
        Serial.printf("[%lu] [WEB] Upload complete: %s (%d bytes)\n", millis(), uploadFileName.c_str(), uploadSize);
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) {
      uploadFile.close();
      // Try to delete the incomplete file
      String filePath = uploadPath;
      if (!filePath.endsWith("/")) filePath += "/";
      filePath += uploadFileName;
      SD.remove(filePath.c_str());
    }
    uploadError = "Upload aborted";
    Serial.printf("[%lu] [WEB] Upload aborted\n", millis());
  }
}

void CrossPointWebServer::handleUploadPost() {
  if (uploadSuccess) {
    server->send(200, "text/plain", "File uploaded successfully: " + uploadFileName);
  } else {
    String error = uploadError.isEmpty() ? "Unknown error during upload" : uploadError;
    server->send(400, "text/plain", error);
  }
}

void CrossPointWebServer::handleCreateFolder() {
  // Get folder name from form data
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing folder name");
    return;
  }

  String folderName = server->arg("name");

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

  Serial.printf("[%lu] [WEB] Creating folder: %s\n", millis(), folderPath.c_str());

  // Check if already exists
  if (SD.exists(folderPath.c_str())) {
    server->send(400, "text/plain", "Folder already exists");
    return;
  }

  // Create the folder
  if (SD.mkdir(folderPath.c_str())) {
    Serial.printf("[%lu] [WEB] Folder created successfully: %s\n", millis(), folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    Serial.printf("[%lu] [WEB] Failed to create folder: %s\n", millis(), folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void CrossPointWebServer::handleDelete() {
  // Get path from form data
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = server->arg("path");
  String itemType = server->hasArg("type") ? server->arg("type") : "file";

  // Validate path
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Cannot delete root directory");
    return;
  }

  // Ensure path starts with /
  if (!itemPath.startsWith("/")) {
    itemPath = "/" + itemPath;
  }

  // Security check: prevent deletion of protected items
  String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

  // Check if item starts with a dot (hidden/system file)
  if (itemName.startsWith(".")) {
    Serial.printf("[%lu] [WEB] Delete rejected - hidden/system item: %s\n", millis(), itemPath.c_str());
    server->send(403, "text/plain", "Cannot delete system files");
    return;
  }

  // Check against explicitly protected items
  for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
    if (itemName.equals(HIDDEN_ITEMS[i])) {
      Serial.printf("[%lu] [WEB] Delete rejected - protected item: %s\n", millis(), itemPath.c_str());
      server->send(403, "text/plain", "Cannot delete protected items");
      return;
    }
  }

  // Check if item exists
  if (!SD.exists(itemPath.c_str())) {
    Serial.printf("[%lu] [WEB] Delete failed - item not found: %s\n", millis(), itemPath.c_str());
    server->send(404, "text/plain", "Item not found");
    return;
  }

  Serial.printf("[%lu] [WEB] Attempting to delete %s: %s\n", millis(), itemType.c_str(), itemPath.c_str());

  bool success = false;

  if (itemType == "folder") {
    // For folders, try to remove (will fail if not empty)
    File dir = SD.open(itemPath.c_str());
    if (dir && dir.isDirectory()) {
      // Check if folder is empty
      File entry = dir.openNextFile();
      if (entry) {
        // Folder is not empty
        entry.close();
        dir.close();
        Serial.printf("[%lu] [WEB] Delete failed - folder not empty: %s\n", millis(), itemPath.c_str());
        server->send(400, "text/plain", "Folder is not empty. Delete contents first.");
        return;
      }
      dir.close();
    }
    success = SD.rmdir(itemPath.c_str());
  } else {
    // For files, use remove
    success = SD.remove(itemPath.c_str());
  }

  if (success) {
    Serial.printf("[%lu] [WEB] Successfully deleted: %s\n", millis(), itemPath.c_str());
    server->send(200, "text/plain", "Deleted successfully");
  } else {
    Serial.printf("[%lu] [WEB] Failed to delete: %s\n", millis(), itemPath.c_str());
    server->send(500, "text/plain", "Failed to delete item");
  }
}
