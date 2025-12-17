#include "CrossPointWebServer.h"

#include <SD.h>
#include <WiFi.h>
#include <algorithm>

#include "config.h"

// Global instance
CrossPointWebServer crossPointWebServer;

// Folders/files to hide from the web interface file browser
// Note: Items starting with "." are automatically hidden
static const char* HIDDEN_ITEMS[] = {
  "System Volume Information",
  "XTCache"
};
static const size_t HIDDEN_ITEMS_COUNT = sizeof(HIDDEN_ITEMS) / sizeof(HIDDEN_ITEMS[0]);

// Helper function to escape HTML special characters to prevent XSS
static String escapeHtml(const String& input) {
  String output;
  output.reserve(input.length() * 1.1);  // Pre-allocate with some extra space
  
  for (size_t i = 0; i < input.length(); i++) {
    char c = input.charAt(i);
    switch (c) {
      case '&':  output += "&amp;";  break;
      case '<':  output += "&lt;";   break;
      case '>':  output += "&gt;";   break;
      case '"':  output += "&quot;"; break;
      case '\'': output += "&#39;";  break;
      default:   output += c;        break;
    }
  }
  return output;
}

// HTML page template
static const char* HTML_PAGE = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>CrossPoint Reader</title>
  <style>
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, sans-serif;
      max-width: 800px;
      margin: 0 auto;
      padding: 20px;
      background-color: #f5f5f5;
      color: #333;
    }
    h1 {
      color: #2c3e50;
      border-bottom: 2px solid #3498db;
      padding-bottom: 10px;
    }
    h2 {
      color: #34495e;
      margin-top: 0;
    }
    .card {
      background: white;
      border-radius: 8px;
      padding: 20px;
      margin: 15px 0;
      box-shadow: 0 2px 4px rgba(0,0,0,0.1);
    }
    .info-row {
      display: flex;
      justify-content: space-between;
      padding: 8px 0;
      border-bottom: 1px solid #eee;
    }
    .info-row:last-child {
      border-bottom: none;
    }
    .label {
      font-weight: 600;
      color: #7f8c8d;
    }
    .value {
      color: #2c3e50;
    }
    .status {
      display: inline-block;
      padding: 4px 12px;
      border-radius: 12px;
      background-color: #27ae60;
      color: white;
      font-size: 0.9em;
    }
    .nav-links {
      margin: 20px 0;
    }
    .nav-links a {
      display: inline-block;
      padding: 10px 20px;
      background-color: #3498db;
      color: white;
      text-decoration: none;
      border-radius: 4px;
      margin-right: 10px;
    }
    .nav-links a:hover {
      background-color: #2980b9;
    }
  </style>
</head>
<body>
  <h1>📚 CrossPoint Reader</h1>
  
  <div class="nav-links">
    <a href="/">Home</a>
    <a href="/files">File Manager</a>
  </div>
  
  <div class="card">
    <h2>Device Status</h2>
    <div class="info-row">
      <span class="label">Version</span>
      <span class="value">%VERSION%</span>
    </div>
    <div class="info-row">
      <span class="label">WiFi Status</span>
      <span class="status">Connected</span>
    </div>
    <div class="info-row">
      <span class="label">IP Address</span>
      <span class="value">%IP_ADDRESS%</span>
    </div>
    <div class="info-row">
      <span class="label">Free Memory</span>
      <span class="value">%FREE_HEAP% bytes</span>
    </div>
  </div>

  <div class="card">
    <p style="text-align: center; color: #95a5a6; margin: 0;">
      CrossPoint E-Reader • Open Source
    </p>
  </div>
</body>
</html>
)rawliteral";

// File listing page template
static const char* FILES_PAGE_HEADER = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>CrossPoint Reader - Files</title>
  <style>
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, sans-serif;
      max-width: 800px;
      margin: 0 auto;
      padding: 20px;
      background-color: #f5f5f5;
      color: #333;
    }
    h1 {
      color: #2c3e50;
      border-bottom: 2px solid #3498db;
      padding-bottom: 10px;
    }
    h2 {
      color: #34495e;
      margin-top: 0;
    }
    .card {
      background: white;
      border-radius: 8px;
      padding: 20px;
      margin: 15px 0;
      box-shadow: 0 2px 4px rgba(0,0,0,0.1);
    }
    .nav-links {
      margin: 20px 0;
    }
    .nav-links a {
      display: inline-block;
      padding: 10px 20px;
      background-color: #3498db;
      color: white;
      text-decoration: none;
      border-radius: 4px;
      margin-right: 10px;
    }
    .nav-links a:hover {
      background-color: #2980b9;
    }
    .file-table {
      width: 100%;
      border-collapse: collapse;
      margin-top: 10px;
    }
    .file-table th, .file-table td {
      padding: 12px;
      text-align: left;
      border-bottom: 1px solid #eee;
    }
    .file-table th {
      background-color: #f8f9fa;
      font-weight: 600;
      color: #7f8c8d;
    }
    .file-table tr:hover {
      background-color: #f8f9fa;
    }
    .epub-file {
      background-color: #e8f6e9 !important;
    }
    .epub-file:hover {
      background-color: #d4edda !important;
    }
    .folder-row {
      background-color: #fff9e6 !important;
    }
    .folder-row:hover {
      background-color: #fff3cd !important;
    }
    .epub-badge {
      display: inline-block;
      padding: 2px 8px;
      background-color: #27ae60;
      color: white;
      border-radius: 10px;
      font-size: 0.75em;
      margin-left: 8px;
    }
    .folder-badge {
      display: inline-block;
      padding: 2px 8px;
      background-color: #f39c12;
      color: white;
      border-radius: 10px;
      font-size: 0.75em;
      margin-left: 8px;
    }
    .file-icon {
      margin-right: 8px;
    }
    .folder-link {
      color: #2c3e50;
      text-decoration: none;
      cursor: pointer;
    }
    .folder-link:hover {
      color: #3498db;
      text-decoration: underline;
    }
    .breadcrumb {
      padding: 10px 15px;
      background-color: #f8f9fa;
      border-radius: 4px;
      margin-bottom: 15px;
    }
    .breadcrumb a {
      color: #3498db;
      text-decoration: none;
    }
    .breadcrumb a:hover {
      text-decoration: underline;
    }
    .breadcrumb span {
      color: #7f8c8d;
      margin: 0 5px;
    }
    .upload-form {
      margin-top: 15px;
      padding: 15px;
      background-color: #f8f9fa;
      border-radius: 4px;
      border: 2px dashed #ddd;
    }
    .upload-form input[type="file"] {
      margin: 10px 0;
    }
    .upload-btn {
      background-color: #27ae60;
      color: white;
      padding: 10px 20px;
      border: none;
      border-radius: 4px;
      cursor: pointer;
      font-size: 1em;
    }
    .upload-btn:hover {
      background-color: #219a52;
    }
    .upload-btn:disabled {
      background-color: #95a5a6;
      cursor: not-allowed;
    }
    .file-info {
      color: #7f8c8d;
      font-size: 0.85em;
      margin-top: 5px;
    }
    .no-files {
      text-align: center;
      color: #95a5a6;
      padding: 40px;
      font-style: italic;
    }
    .message {
      padding: 15px;
      border-radius: 4px;
      margin: 15px 0;
    }
    .message.success {
      background-color: #d4edda;
      color: #155724;
      border: 1px solid #c3e6cb;
    }
    .message.error {
      background-color: #f8d7da;
      color: #721c24;
      border: 1px solid #f5c6cb;
    }
    .summary {
      display: flex;
      justify-content: space-between;
      padding: 10px 0;
      border-bottom: 2px solid #eee;
      margin-bottom: 10px;
    }
    .summary-item {
      text-align: center;
    }
    .summary-number {
      font-size: 1.5em;
      font-weight: bold;
      color: #2c3e50;
    }
    .summary-label {
      font-size: 0.85em;
      color: #7f8c8d;
    }
    #progress-container {
      display: none;
      margin-top: 10px;
    }
    #progress-bar {
      width: 100%;
      height: 20px;
      background-color: #e0e0e0;
      border-radius: 10px;
      overflow: hidden;
    }
    #progress-fill {
      height: 100%;
      background-color: #27ae60;
      width: 0%;
      transition: width 0.3s;
    }
    #progress-text {
      text-align: center;
      margin-top: 5px;
      font-size: 0.9em;
      color: #7f8c8d;
    }
    .folder-form {
      display: flex;
      gap: 10px;
      margin-top: 15px;
    }
    .folder-input {
      flex: 1;
      padding: 10px;
      border: 1px solid #ddd;
      border-radius: 4px;
      font-size: 1em;
    }
    .folder-btn {
      background-color: #f39c12;
      color: white;
      padding: 10px 20px;
      border: none;
      border-radius: 4px;
      cursor: pointer;
      font-size: 1em;
    }
    .folder-btn:hover {
      background-color: #d68910;
    }
  </style>
</head>
<body>
  <h1>📁 File Manager</h1>
  
  <div class="nav-links">
    <a href="/">Home</a>
    <a href="/files">File Manager</a>
  </div>
)rawliteral";

static const char* FILES_PAGE_FOOTER = R"rawliteral(
  <div class="card">
    <p style="text-align: center; color: #95a5a6; margin: 0;">
      CrossPoint E-Reader • Open Source
    </p>
  </div>
  
  <script>
    function validateFile() {
      const fileInput = document.getElementById('fileInput');
      const uploadBtn = document.getElementById('uploadBtn');
      const file = fileInput.files[0];
      
      if (file) {
        const fileName = file.name.toLowerCase();
        if (!fileName.endsWith('.epub')) {
          alert('Only .epub files are allowed!');
          fileInput.value = '';
          uploadBtn.disabled = true;
          return;
        }
        uploadBtn.disabled = false;
      } else {
        uploadBtn.disabled = true;
      }
    }
    
    function uploadFile() {
      const fileInput = document.getElementById('fileInput');
      const file = fileInput.files[0];
      const currentPath = document.getElementById('currentPath').value;
      
      if (!file) {
        alert('Please select a file first!');
        return;
      }
      
      const fileName = file.name.toLowerCase();
      if (!fileName.endsWith('.epub')) {
        alert('Only .epub files are allowed!');
        return;
      }
      
      const formData = new FormData();
      formData.append('file', file);
      formData.append('path', currentPath);
      
      const progressContainer = document.getElementById('progress-container');
      const progressFill = document.getElementById('progress-fill');
      const progressText = document.getElementById('progress-text');
      const uploadBtn = document.getElementById('uploadBtn');
      
      progressContainer.style.display = 'block';
      uploadBtn.disabled = true;
      
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/upload', true);
      
      xhr.upload.onprogress = function(e) {
        if (e.lengthComputable) {
          const percent = Math.round((e.loaded / e.total) * 100);
          progressFill.style.width = percent + '%';
          progressText.textContent = 'Uploading: ' + percent + '%';
        }
      };
      
      xhr.onload = function() {
        if (xhr.status === 200) {
          progressText.textContent = 'Upload complete!';
          setTimeout(function() {
            window.location.reload();
          }, 1000);
        } else {
          progressText.textContent = 'Upload failed: ' + xhr.responseText;
          progressFill.style.backgroundColor = '#e74c3c';
          uploadBtn.disabled = false;
        }
      };
      
      xhr.onerror = function() {
        progressText.textContent = 'Upload failed - network error';
        progressFill.style.backgroundColor = '#e74c3c';
        uploadBtn.disabled = false;
      };
      
      xhr.send(formData);
    }
    
    function createFolder() {
      const folderName = document.getElementById('folderName').value.trim();
      const currentPath = document.getElementById('currentPath').value;
      
      if (!folderName) {
        alert('Please enter a folder name!');
        return;
      }
      
      // Validate folder name (no special characters except underscore and hyphen)
      const validName = /^[a-zA-Z0-9_\-]+$/.test(folderName);
      if (!validName) {
        alert('Folder name can only contain letters, numbers, underscores, and hyphens.');
        return;
      }
      
      const formData = new FormData();
      formData.append('name', folderName);
      formData.append('path', currentPath);
      
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/mkdir', true);
      
      xhr.onload = function() {
        if (xhr.status === 200) {
          window.location.reload();
        } else {
          alert('Failed to create folder: ' + xhr.responseText);
        }
      };
      
      xhr.onerror = function() {
        alert('Failed to create folder - network error');
      };
      
      xhr.send(formData);
    }
  </script>
</body>
</html>
)rawliteral";

CrossPointWebServer::CrossPointWebServer() {}

CrossPointWebServer::~CrossPointWebServer() {
  stop();
}

void CrossPointWebServer::begin() {
  if (running) {
    Serial.printf("[%lu] [WEB] Web server already running\n", millis());
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[%lu] [WEB] Cannot start webserver - WiFi not connected\n", millis());
    return;
  }

  Serial.printf("[%lu] [WEB] Creating web server on port %d...\n", millis(), port);
  server = new WebServer(port);

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
  
  server->onNotFound([this]() { handleNotFound(); });

  server->begin();
  running = true;

  Serial.printf("[%lu] [WEB] Web server started on port %d\n", millis(), port);
  Serial.printf("[%lu] [WEB] Access at http://%s/\n", millis(), WiFi.localIP().toString().c_str());
}

void CrossPointWebServer::stop() {
  if (!running || !server) {
    return;
  }

  server->stop();
  delete server;
  server = nullptr;
  running = false;

  Serial.printf("[%lu] [WEB] Web server stopped\n", millis());
}

void CrossPointWebServer::handleClient() {
  static unsigned long lastDebugPrint = 0;
  if (running && server) {
    // Print debug every 10 seconds to confirm handleClient is being called
    if (millis() - lastDebugPrint > 10000) {
      Serial.printf("[%lu] [WEB] handleClient active, server running on port %d\n", millis(), port);
      lastDebugPrint = millis();
    }
    server->handleClient();
  }
}

void CrossPointWebServer::handleRoot() {
  String html = HTML_PAGE;

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
  String html = FILES_PAGE_HEADER;
  
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
  
  // Breadcrumb navigation
  html += "<div class=\"card\">";
  html += "<div class=\"breadcrumb\">";
  html += "<a href=\"/files\">🏠 Root</a>";
  
  if (currentPath != "/") {
    String pathParts = currentPath.substring(1); // Remove leading /
    String buildPath = "";
    int start = 0;
    int end = pathParts.indexOf('/');
    
    while (start < pathParts.length()) {
      String part;
      if (end == -1) {
        part = pathParts.substring(start);
        buildPath += "/" + part;
        html += "<span>/</span><strong>" + part + "</strong>";
        break;
      } else {
        part = pathParts.substring(start, end);
        buildPath += "/" + part;
        html += "<span>/</span><a href=\"/files?path=" + buildPath + "\">" + part + "</a>";
        start = end + 1;
        end = pathParts.indexOf('/', start);
      }
    }
  }
  html += "</div>";
  html += "</div>";
  
  // Upload form
  html += "<div class=\"card\">";
  html += "<h2>📤 Upload eBook to " + (currentPath == "/" ? "Root" : currentPath) + "</h2>";
  html += "<div class=\"upload-form\">";
  html += "<p><strong>Select an .epub file to upload:</strong></p>";
  html += "<input type=\"file\" id=\"fileInput\" accept=\".epub\" onchange=\"validateFile()\">";
  html += "<div class=\"file-info\">Only .epub files are accepted. File will be uploaded to: " + currentPath + "</div>";
  html += "<button id=\"uploadBtn\" class=\"upload-btn\" onclick=\"uploadFile()\" disabled>Upload</button>";
  html += "<div id=\"progress-container\">";
  html += "<div id=\"progress-bar\"><div id=\"progress-fill\"></div></div>";
  html += "<div id=\"progress-text\"></div>";
  html += "</div>";
  html += "</div>";
  
  // Create folder form
  html += "<div class=\"folder-form\">";
  html += "<input type=\"text\" id=\"folderName\" class=\"folder-input\" placeholder=\"New folder name...\">";
  html += "<button class=\"folder-btn\" onclick=\"createFolder()\">📁 Create Folder</button>";
  html += "</div>";
  html += "</div>";
  
  // Scan files in current path
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
  
  // File listing
  html += "<div class=\"card\">";
  html += "<h2>📁 Contents of " + (currentPath == "/" ? "Root" : currentPath) + "</h2>";
  
  // Summary
  html += "<div class=\"summary\">";
  html += "<div class=\"summary-item\"><div class=\"summary-number\">" + String(folderCount) + "</div><div class=\"summary-label\">Folders</div></div>";
  html += "<div class=\"summary-item\"><div class=\"summary-number\">" + String(files.size() - folderCount) + "</div><div class=\"summary-label\">Files</div></div>";
  html += "<div class=\"summary-item\"><div class=\"summary-number\">" + String(epubCount) + "</div><div class=\"summary-label\">eBooks</div></div>";
  html += "<div class=\"summary-item\"><div class=\"summary-number\">" + formatFileSize(totalSize) + "</div><div class=\"summary-label\">Total Size</div></div>";
  html += "</div>";
  
  if (files.empty()) {
    html += "<div class=\"no-files\">This folder is empty</div>";
  } else {
    html += "<table class=\"file-table\">";
    html += "<tr><th>Name</th><th>Type</th><th>Size</th></tr>";
    
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
        html += "<a href=\"/files?path=" + folderPath + "\" class=\"folder-link\">" + file.name + "</a>" + badge + "</td>";
        html += "<td>" + typeStr + "</td>";
        html += "<td>" + sizeStr + "</td>";
        html += "</tr>";
      } else {
        rowClass = file.isEpub ? "epub-file" : "";
        icon = file.isEpub ? "📗" : "📄";
        badge = file.isEpub ? "<span class=\"epub-badge\">EPUB</span>" : "";
        String ext = file.name.substring(file.name.lastIndexOf('.') + 1);
        ext.toUpperCase();
        typeStr = ext;
        sizeStr = formatFileSize(file.size);
        
        html += "<tr class=\"" + rowClass + "\">";
        html += "<td><span class=\"file-icon\">" + icon + "</span>" + file.name + badge + "</td>";
        html += "<td>" + typeStr + "</td>";
        html += "<td>" + sizeStr + "</td>";
        html += "</tr>";
      }
    }
    
    html += "</table>";
  }
  
  html += "</div>";
  
  html += FILES_PAGE_FOOTER;
  
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
  HTTPUpload& upload = server->upload();
  
  if (upload.status == UPLOAD_FILE_START) {
    uploadFileName = upload.filename;
    uploadSize = 0;
    uploadSuccess = false;
    uploadError = "";
    
    // Get upload path from form data (defaults to root if not specified)
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
    
    Serial.printf("[%lu] [WEB] Upload start: %s to path: %s\n", millis(), uploadFileName.c_str(), uploadPath.c_str());
    
    // Validate file extension
    if (!isEpubFile(uploadFileName)) {
      uploadError = "Only .epub files are allowed";
      Serial.printf("[%lu] [WEB] Upload rejected - not an epub file\n", millis());
      return;
    }
    
    // Create file path
    String filePath = uploadPath;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += uploadFileName;
    
    // Check if file already exists
    if (SD.exists(filePath.c_str())) {
      Serial.printf("[%lu] [WEB] Overwriting existing file: %s\n", millis(), filePath.c_str());
      SD.remove(filePath.c_str());
    }
    
    // Open file for writing
    uploadFile = SD.open(filePath.c_str(), FILE_WRITE);
    if (!uploadFile) {
      uploadError = "Failed to create file on SD card";
      Serial.printf("[%lu] [WEB] Failed to create file: %s\n", millis(), filePath.c_str());
      return;
    }
    
    Serial.printf("[%lu] [WEB] File created: %s\n", millis(), filePath.c_str());
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile && uploadError.isEmpty()) {
      size_t written = uploadFile.write(upload.buf, upload.currentSize);
      if (written != upload.currentSize) {
        uploadError = "Failed to write to SD card - disk may be full";
        uploadFile.close();
        Serial.printf("[%lu] [WEB] Write error - expected %d, wrote %d\n", millis(), upload.currentSize, written);
      } else {
        uploadSize += written;
      }
    }
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      
      if (uploadError.isEmpty()) {
        uploadSuccess = true;
        Serial.printf("[%lu] [WEB] Upload complete: %s (%d bytes)\n", millis(), uploadFileName.c_str(), uploadSize);
      }
    }
  }
  else if (upload.status == UPLOAD_FILE_ABORTED) {
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
