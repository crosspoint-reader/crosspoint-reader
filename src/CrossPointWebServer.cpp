#include "CrossPointWebServer.h"

#include <WiFi.h>

#include "config.h"

// Global instance
CrossPointWebServer crossPointWebServer;

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
      max-width: 600px;
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
    .coming-soon {
      color: #95a5a6;
      font-style: italic;
      text-align: center;
      padding: 20px;
    }
  </style>
</head>
<body>
  <h1>📚 CrossPoint Reader</h1>
  
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
    <h2>File Management</h2>
    <p class="coming-soon">📁 File upload functionality coming soon...</p>
  </div>

  <div class="card">
    <p style="text-align: center; color: #95a5a6; margin: 0;">
      CrossPoint E-Reader • Open Source
    </p>
  </div>
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
